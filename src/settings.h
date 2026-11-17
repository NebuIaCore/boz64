// The port's own options, kept in <app files dir>/boz64.cfg as "name=value" lines:
//   fps_limit       frames per second, 0 = uncapped (default)
//   res_scale       percent of the screen's resolution the 3D view is drawn at (25-100, default 100)
//   zombies_ignore  1 = the AI never targets the local player (default 0)
//   mouse_sensitivity  mouse look speed, as in Call of Duty on PC (0.022 degrees per count each; default 5)
//   mouse_ads       mouse look speed while aiming down the sights, percent of the above (default 60)
// For testing, the property debug.boz64.<name> overrides a value while it is set.
#pragma once
#include <string>

#include "common.h"

struct Settings {
    int fps_limit = 0;
    int res_scale = 100;
    bool zombies_ignore = false;
    float mouse_sensitivity = 5.0f;
    int mouse_ads = 60;
};

void settings_init(const std::string& files_dir);
Settings settings();                   // the current values (cheap)
void settings_poll();                  // call once per frame; picks up changes to the file or properties
void settings_save(const Settings& s);  // change the values and write the file
void settings_set(const Settings& s, bool write_file);  // change the values (in use at once), optionally writing the file
