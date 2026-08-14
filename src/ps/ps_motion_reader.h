#pragma once

#ifdef __SWITCH__

namespace lunar::ps {

struct PsMotionState {
    bool valid = false;
    float gyro_x = 0.0f, gyro_y = 0.0f, gyro_z = 0.0f;
    float accel_x = 0.0f, accel_y = 1.0f, accel_z = 0.0f;
    float orient_x = 0.0f, orient_y = 0.0f, orient_z = 0.0f, orient_w = 1.0f;
};

class PsMotionReader {
public:
    PsMotionReader();
    ~PsMotionReader();
    PsMotionReader(const PsMotionReader&) = delete;
    PsMotionReader& operator=(const PsMotionReader&) = delete;

    bool initialize();
    PsMotionState read(bool suppressed);
    void reset();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace lunar::ps

#endif
