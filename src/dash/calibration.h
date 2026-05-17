// calibration.h — interactive gesture-data capture.
//
// Walks the user through every physical gesture we care about (tap, flick
// in each of four directions, shake), prompts them on the OLED, and
// records the peak linear-accel components seen during each capture
// window. The summary printed to serial reveals which IMU body axis
// dominates for each gesture in this specific cube's mounting — useful
// when bop-it's flick-arrow → flick-direction matcher fails because
// our assumed axis mapping is wrong.

#ifndef DASH_CALIBRATION_H
#define DASH_CALIBRATION_H

namespace dash {

void runCalibration();

}  // namespace dash

#endif
