#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace vox {

constexpr float kPi = 3.14159265358979323846f;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vec3& operator-=(Vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    constexpr Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    constexpr bool operator==(const Vec3&) const = default;
};

constexpr Vec3 operator*(float s, Vec3 v) { return v * s; }

constexpr float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(Vec3 v) {
    const float len = length(v);
    return len > 1.0e-8f ? v / len : Vec3{};
}
constexpr Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
constexpr float lerp(float a, float b, float t) { return a + (b - a) * t; }
constexpr float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Aabb {
    Vec3 min{};
    Vec3 max{};

    constexpr bool contains(Vec3 p) const {
        return p.x >= min.x && p.x <= max.x &&
               p.y >= min.y && p.y <= max.y &&
               p.z >= min.z && p.z <= max.z;
    }

    constexpr bool overlaps(const Aabb& o) const {
        return min.x <= o.max.x && max.x >= o.min.x &&
               min.y <= o.max.y && max.y >= o.min.y &&
               min.z <= o.max.z && max.z >= o.min.z;
    }
};

struct Mat4;

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    static constexpr Quat identity() { return {}; }

    static Quat from_axis_angle(Vec3 axis, float angle) {
        axis = normalize(axis);
        const float half = angle * 0.5f;
        const float s = std::sin(half);
        return Quat{axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
    }

    [[nodiscard]] Quat normalized() const {
        const float len = std::sqrt(x * x + y * y + z * z + w * w);
        if (len < 1.0e-8f) {
            return identity();
        }
        return Quat{x / len, y / len, z / len, w / len};
    }

    [[nodiscard]] Mat4 to_mat4() const;
};

struct Mat4 {
    // Column-major, like OpenGL/Vulkan.
    float m[16]{};

    static constexpr Mat4 identity() {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    static constexpr Mat4 translation(Vec3 t) {
        Mat4 r = identity();
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }

    static constexpr Mat4 scale(Vec3 s) {
        Mat4 r{};
        r.m[0] = s.x;
        r.m[5] = s.y;
        r.m[10] = s.z;
        r.m[15] = 1.0f;
        return r;
    }

    static constexpr Mat4 trs(Vec3 translation, Quat rotation, Vec3 scale) {
        return Mat4::translation(translation) * rotation.to_mat4() * Mat4::scale(scale);
    }

    constexpr Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += m[k * 4 + row] * o.m[col * 4 + k];
                }
                r.m[col * 4 + row] = sum;
            }
        }
        return r;
    }
};

// Right-handed perspective with Vulkan/SDL_GPU clip depth in [0, 1].
inline Vec3 transform_point(const Mat4& matrix, Vec3 point) {
    const float x = matrix.m[0] * point.x + matrix.m[4] * point.y +
                    matrix.m[8] * point.z + matrix.m[12];
    const float y = matrix.m[1] * point.x + matrix.m[5] * point.y +
                    matrix.m[9] * point.z + matrix.m[13];
    const float z = matrix.m[2] * point.x + matrix.m[6] * point.y +
                    matrix.m[10] * point.z + matrix.m[14];
    const float w = matrix.m[3] * point.x + matrix.m[7] * point.y +
                    matrix.m[11] * point.z + matrix.m[15];
    if (std::abs(w) > 1.0e-8f) {
        return Vec3{x / w, y / w, z / w};
    }
    return Vec3{x, y, z};
}

inline Mat4 Quat::to_mat4() const {
    const Quat q = normalized();
    Mat4 r = Mat4::identity();
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;

    r.m[0] = 1.0f - 2.0f * (yy + zz);
    r.m[1] = 2.0f * (xy + wz);
    r.m[2] = 2.0f * (xz - wy);
    r.m[4] = 2.0f * (xy - wz);
    r.m[5] = 1.0f - 2.0f * (xx + zz);
    r.m[6] = 2.0f * (yz + wx);
    r.m[8] = 2.0f * (xz + wy);
    r.m[9] = 2.0f * (yz - wx);
    r.m[10] = 1.0f - 2.0f * (xx + yy);
    return r;
}

inline Mat4 perspective(float fov_y_radians, float aspect, float near_z, float far_z) {
    Mat4 r{};
    const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = far_z / (near_z - far_z);
    r.m[11] = -1.0f;
    r.m[14] = (far_z * near_z) / (near_z - far_z);
    return r;
}

inline Mat4 look_at(Vec3 eye, Vec3 center, Vec3 up) {
    const Vec3 f = normalize(center - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);

    Mat4 r = Mat4::identity();
    r.m[0] = s.x;
    r.m[1] = u.x;
    r.m[2] = -f.x;
    r.m[4] = s.y;
    r.m[5] = u.y;
    r.m[6] = -f.y;
    r.m[8] = s.z;
    r.m[9] = u.z;
    r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = dot(f, eye);
    return r;
}

} // namespace vox
