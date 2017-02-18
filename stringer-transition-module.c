#include <obs-module.h>
#include <obs-frontend-api.h>

OBS_DECLARE_MODULE()

OBS_MODULE_USE_DEFAULT_LOCALE("obs-transitions", "en-US") //might be useful in future but not used right now

extern struct obs_source_info stinger_transition;

bool obs_module_load(void)
{
	obs_register_source(&stinger_transition);
	return true;
}

bool obs_module_unload()
{
	return true;
}