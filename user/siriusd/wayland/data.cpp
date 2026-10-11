// wl_data_device_manager: the clipboard and drag and drop between clients.
//
// Present, because toolkits expect it, and inert: data sources are accepted
// and never offered to anyone, so a copy succeeds and a paste finds nothing.
// Sharing a selection -- between clients, and with the desktop's own
// applications -- is work for later.
#include "internal.h"

namespace wayland {
namespace {

void Destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void SourceOffer(wl_client*, wl_resource*, const char*) {}
void SourceSetActions(wl_client*, wl_resource*, uint32_t) {}
const struct wl_data_source_interface kSourceImpl = { SourceOffer, Destroy, SourceSetActions };

void DeviceStartDrag(wl_client*, wl_resource*, wl_resource*, wl_resource*, wl_resource*, uint32_t) {}
// The source is never used, and the protocol has the client destroy it.
void DeviceSetSelection(wl_client*, wl_resource*, wl_resource*, uint32_t) {}
const struct wl_data_device_interface kDeviceImpl = { DeviceStartDrag, DeviceSetSelection, Destroy };

void ManagerCreateSource(wl_client* client, wl_resource* manager, uint32_t id)
{
    wl_resource* resource = wl_resource_create(client, &wl_data_source_interface, wl_resource_get_version(manager), id);
    if (resource == nullptr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &kSourceImpl, nullptr, nullptr);
}

void ManagerGetDevice(wl_client* client, wl_resource* manager, uint32_t id, wl_resource*)
{
    wl_resource* resource = wl_resource_create(client, &wl_data_device_interface, wl_resource_get_version(manager), id);
    if (resource == nullptr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &kDeviceImpl, nullptr, nullptr);
}

// release (version 4, in newer libwaylands) stays null: version 3 is advertised.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const struct wl_data_device_manager_interface kManagerImpl = { .create_data_source = ManagerCreateSource, .get_data_device = ManagerGetDevice };
#pragma GCC diagnostic pop

void BindManager(wl_client* client, void*, uint32_t version, uint32_t id)
{
    wl_resource* resource = wl_resource_create(client, &wl_data_device_manager_interface, (int)version, id);
    if (resource == nullptr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &kManagerImpl, nullptr, nullptr);
}

} // namespace

bool CreateDataDeviceGlobals(wl_display* display)
{
    return wl_global_create(display, &wl_data_device_manager_interface, 3, nullptr, BindManager) != nullptr;
}

} // namespace wayland
