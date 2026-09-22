#include "Shunt.h"

/**
    The effect: the same train, over or cut into the incoming clip.

    Read the note in SourcePlugin.cpp on why this file is listed directly in its
    own target rather than in `shunt_core`. The short version is that the
    `CFFGLPluginInfo` below is the one thing the two plugins must NOT share.

    The plugin ID differs from the source's, and it has to: Resolume keys a
    saved composition's effect to that ID, so two plugins sharing one would make
    a composition ambiguous about which of them it meant.
*/
namespace
{
class ShuntEffect : public shunt::ShuntPlugin
{
public:
	ShuntEffect() :
		ShuntPlugin( true )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< ShuntEffect >,                     // Create method
	"SH02",                                           // Plugin unique ID of maximum length 4
	"SW Shunt Mask",                                  // Plugin name
	2,                                                // API major version number
	1,                                                // API minor version number
	0,                                                // Plugin major version number
	1,                                                // Plugin minor version number
	FF_EFFECT,                                        // Plugin type
	"A queue of shapes over the clip, or cut into it.\n\nShapes slide in from an edge, bank up a set distance apart, stand, and are drawn away. Gap can be set in multiples of the shape's own thickness or in absolute pixels.\n\nDwell is the one dial: turn it down and the front of the queue is still leaving as the back arrives; turn it up and the whole train stands before anything moves.\n\nStart from a Preset, at the bottom.",// Plugin description
	"Shunt FFGL effect"                               // About
);

extern "C" const char* ShuntEffectBuildStamp()
{
	return "shunt " SHUNT_VERSION " effect, built " __DATE__ " " __TIME__;
}
