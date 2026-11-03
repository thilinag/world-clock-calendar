// Speaker alert via the on-board ES8311 codec (I2S) + NS4150 amp.
#pragma once

bool audioBegin();        // call once in setup(), after Wire.begin()
void audioPlayAlert();    // non-blocking: plays the chime on a background task
