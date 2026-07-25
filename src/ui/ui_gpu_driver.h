#ifndef __UI_GPU_DRIVER_H__
#define __UI_GPU_DRIVER_H__

#include "RmlUi/Core.h"

namespace recompui {
    class UiEventListenerInstancer;

    // The "GPU Driver" settings category: a user-supplied Vulkan driver, which
    // exists on Android and only in a build with the loader compiled in. The data
    // model is always created -- the document references it either way -- and
    // `driver_supported` is what hides the whole tab everywhere else.
    void make_gpu_driver_bindings(Rml::Context* context);
    void register_gpu_driver_events(UiEventListenerInstancer& listener);
}

#endif
