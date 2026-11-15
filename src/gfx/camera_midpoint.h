// DawnOfTermina — native static recompilation of Majora's Mask 3D.
// Copyright (C) 2026 DawnOfTermina contributors
// Camera math ported from DekUI (src/mm3d/camera_midpoint.h, "MM3D presentation contributors",
// GPL-2.0-or-later): https://github.com/tgacquin/mm3d-dekui
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// The world shader (GzwShader) takes the projection in c0-c3, the bone palette in view space in
// c20+3b (3 rows per bone, 10 bones) and the camera-to-world matrix in c70-c72 (rows; translation
// = eye). Interpolating the camera as a rigid motion (quaternion midpoint) instead of averaging
// view-space matrices keeps the scene from shrinking during fast pans, and the camera tells
// cuts apart from motion.
namespace gfx::camera {

using Mat = std::array<std::array<double, 4>, 3>;  // 3x4 affine, implicit last row 0 0 0 1

inline Mat mul(const Mat& a, const Mat& b) {
    Mat r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            double v = j == 3 ? a[i][3] : 0;
            for (int k = 0; k < 3; ++k) v += a[i][k] * b[k][j];
            r[i][j] = v;
        }
    return r;
}

inline bool rigid(const Mat& m) {
    for (const auto& row : m)
        for (double v : row)
            if (!std::isfinite(v)) return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double dot = 0;
            for (int k = 0; k < 3; ++k) dot += m[k][i] * m[k][j];
            if (std::abs(dot - (i == j ? 1 : 0)) > .002) return false;
        }
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    return std::abs(det - 1) < .003;
}

inline Mat inverse_rigid(const Mat& m) {
    Mat r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) r[i][j] = m[j][i];
        for (int j = 0; j < 3; ++j) r[i][3] -= r[i][j] * m[j][3];
    }
    return r;
}

inline std::array<double, 4> rotation(const Mat& m) {  // quaternion w, x, y, z
    std::array<double, 4> q{};
    const double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0) {
        const double s = 2 * std::sqrt(trace + 1);
        q = {s / 4, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
    } else {
        int i = 0;
        for (int k = 1; k < 3; ++k)
            if (m[k][k] > m[i][i]) i = k;
        const int j = (i + 1) % 3, k = (i + 2) % 3;
        const double s = 2 * std::sqrt(1 + m[i][i] - m[j][j] - m[k][k]);
        q[0] = (m[k][j] - m[j][k]) / s, q[i + 1] = s / 4;
        q[j + 1] = (m[j][i] + m[i][j]) / s, q[k + 1] = (m[k][i] + m[i][k]) / s;
    }
    double len = 0;
    for (double v : q) len += v * v;
    for (double& v : q) v /= std::sqrt(len);
    return q;
}

// Rigid motion a fraction t of the way from a to b: rotation by slerp (constant angular speed,
// so 2 or 3 in-between frames are evenly spaced), translation linearly.
inline Mat lerp(const Mat& a, const Mat& b, double t) {
    const auto qa = rotation(a);
    auto qb = rotation(b);
    double dot = 0, len = 0;
    for (int i = 0; i < 4; ++i) dot += qa[i] * qb[i];
    if (dot < 0)
        for (double& v : qb) v = -v;
    dot = std::min(std::abs(dot), 1.0);
    const double angle = std::acos(dot), s = std::sin(angle);
    const double ka = s > 1e-6 ? std::sin((1 - t) * angle) / s : 1 - t, kb = s > 1e-6 ? std::sin(t * angle) / s : t;
    std::array<double, 4> q;
    for (int i = 0; i < 4; ++i) q[i] = ka * qa[i] + kb * qb[i], len += q[i] * q[i];
    for (double& v : q) v /= std::sqrt(len);
    const auto [w, x, y, z] = q;
    Mat r{{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0},
           {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0},
           {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0}}};
    for (int i = 0; i < 3; ++i) r[i][3] = a[i][3] + t * (b[i][3] - a[i][3]);
    return r;
}

// DekUI's cut test: more than 250 units of travel or 45° of rotation in one 30 Hz frame.
inline bool cut(const Mat& a, const Mat& b) {
    double dist = 0, trace = 0;
    for (int i = 0; i < 3; ++i) {
        dist += (a[i][3] - b[i][3]) * (a[i][3] - b[i][3]);
        for (int j = 0; j < 3; ++j) trace += a[i][j] * b[i][j];
    }
    return dist > 250.0 * 250.0 || (trace - 1) / 2 < std::cos(45.0 * 3.14159265358979323846 / 180.0);
}

}  // namespace gfx::camera
