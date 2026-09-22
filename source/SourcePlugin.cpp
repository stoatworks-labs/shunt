#include "Shunt.h"

/**
    The generator: the train over its own background, no input.

    **This file is listed directly in the ShuntSource target, not in
    shunt_core.** Both plugins share the class; what they do not share is the
    `CFFGLPluginInfo` below, and putting either registration in the shared
    library would register both plugins into both bundles.

    It is also why the shared library is an OBJECT library rather than a STATIC
    one. `CFFGLPluginInfo` registers itself from a file-scope constructor and
    nothing ever references it by name, so in an archive the linker is entitled
    to drop the whole translation unit -- giving a bundle that loads, exports
    `plugMain`, and reports that it contains no plugins.

        nm -gU Shunt.bundle/Contents/MacOS/Shunt | grep plugMain
*/
namespace
{
class ShuntSource : public shunt::ShuntPlugin
{
public:
	ShuntSource() :
		ShuntPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< ShuntSource >,                       // Create method
	"SH01",                                             // Plugin unique ID of maximum length 4
	"SW Shunt",                                         // Plugin name
	2,                                                  // API major version number
	1,                                                  // API minor version number
	0,                                                  // Plugin major version number
	1,                                                  // Plugin minor version number
	FF_SOURCE,                                          // Plugin type
	"Shapes slide in from an edge, bank up a set distance apart, stand, and are drawn away.\n\nFor animated masks and for chroma animations driving a pixel map. Gap can be set in multiples of the shape's own thickness or in absolute pixels.\n\nDwell is the one dial: turn it down and the front of the queue is still leaving as the back arrives; turn it up and the whole train stands before anything moves.\n\nStart from a Preset, at the bottom.",// Plugin description
	"Shunt FFGL source"                                 // About
);

extern "C" const char* ShuntSourceBuildStamp()
{
	return "shunt " SHUNT_VERSION " source, built " __DATE__ " " __TIME__;
}
