// wl_compositor, wl_surface and wl_region: rectangles of pixels that clients
// fill from shared memory and commit.
//
// A commit copies the buffer into memory of the compositor's own and
// releases it at once: the copy is what gets drawn, for as many frames as it
// takes, and the client has its buffer back for the next frame. Damage is
// accepted and ignored; the whole buffer is copied. Opaque and input
// regions are ignored too: every pixel of a surface takes input, and alpha
// decides what is drawn.
#include "internal.h"

#include <algorithm>
#include <cstring>

namespace wayland {
namespace {

std::vector<Surface*> g_surfaces;

void ForgetPendingBuffer(Surface* surface)
{
    if (surface->Pending.Buffer != nullptr)
    {
        wl_list_remove(&surface->BufferGone.Listener.link);
        surface->Pending.Buffer = nullptr;
    }
}

void PendingBufferDestroyed(wl_listener* listener, void*)
{
    // The listener is the first member of a BufferListener.
    auto* owner = reinterpret_cast<Surface::BufferListener*>(listener)->Owner;
    wl_list_remove(&listener->link);
    owner->Pending.Buffer = nullptr;
}

void DestroyCallbacks(wl_list* list)
{
    wl_resource* callback;
    wl_resource* next;
    wl_resource_for_each_safe(callback, next, list)
        wl_resource_destroy(callback);
}

// Copy a client's buffer into the surface. False if it is not a buffer this
// compositor can read.
bool TakeBuffer(Surface* surface, wl_resource* buffer)
{
    wl_shm_buffer* shm = wl_shm_buffer_get(buffer);
    if (shm == nullptr)
    {
        wl_resource_post_error(surface->Resource, WL_SURFACE_ERROR_INVALID_SIZE, "only wl_shm buffers are supported");
        return false;
    }
    const uint32_t format = wl_shm_buffer_get_format(shm);
    const int width = wl_shm_buffer_get_width(shm), height = wl_shm_buffer_get_height(shm);
    const int stride = wl_shm_buffer_get_stride(shm);
    if (width <= 0 || height <= 0 || stride < width * 4)
        return false;

    surface->Pixels.resize((size_t)width * height);
    // Reading the client's memory can fault if it has shrunk the pool
    // under us; begin_access turns that into an error for the client.
    wl_shm_buffer_begin_access(shm);
    const auto* data = static_cast<const unsigned char*>(wl_shm_buffer_get_data(shm));
    for (int y = 0; y < height; y++)
        std::memcpy(&surface->Pixels[(size_t)y * width], data + (size_t)y * stride, (size_t)width * 4);
    wl_shm_buffer_end_access(shm);

    surface->Width = width;
    surface->Height = height;
    surface->HasContent = true;
    surface->Texture.Width = width;
    surface->Texture.Height = height;
    surface->Texture.Format = ImTextureFormat_RGBA32;
    surface->Texture.Pixels = reinterpret_cast<const unsigned char*>(surface->Pixels.data());
    surface->Texture.Layout = format == WL_SHM_FORMAT_ARGB8888 ? ImGui_ImplSoftRast_Layout_BGRA_Premultiplied
                                                               : ImGui_ImplSoftRast_Layout_BGRX;
    return true;
}

// --- wl_surface ----------------------------------------------------------------------

void SurfaceDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

void SurfaceAttach(wl_client*, wl_resource* resource, wl_resource* buffer, int32_t x, int32_t y)
{
    Surface* surface = SurfaceFrom(resource);
    ForgetPendingBuffer(surface);
    if (buffer != nullptr)
    {
        surface->Pending.Buffer = buffer;
        wl_resource_add_destroy_listener(buffer, &surface->BufferGone.Listener);
    }
    surface->Pending.Attached = true;
    surface->Pending.Dx = x;
    surface->Pending.Dy = y;
}

void SurfaceDamage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}

void SurfaceFrame(wl_client* client, wl_resource* resource, uint32_t id)
{
    Surface* surface = SurfaceFrom(resource);
    wl_resource* callback = wl_resource_create(client, &wl_callback_interface, 1, id);
    if (callback == nullptr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(callback, nullptr, nullptr,
                                   [](wl_resource* r) { wl_list_remove(wl_resource_get_link(r)); });
    wl_list_insert(surface->Pending.Frames.prev, wl_resource_get_link(callback));
}

void SurfaceSetRegion(wl_client*, wl_resource*, wl_resource*) {}

void SurfaceCommit(wl_client*, wl_resource* resource)
{
    Surface* surface = SurfaceFrom(resource);
    if (surface->Pending.Attached)
    {
        wl_resource* buffer = surface->Pending.Buffer;
        ForgetPendingBuffer(surface);
        if (buffer == nullptr || !TakeBuffer(surface, buffer))
        {
            surface->HasContent = false;
            surface->Width = surface->Height = 0;
        }
        if (buffer != nullptr)
            wl_buffer_send_release(buffer);
        surface->Pending.Attached = false;
        surface->NewContent = true;
    }
    wl_list_insert_list(surface->Frames.prev, &surface->Pending.Frames);
    wl_list_init(&surface->Pending.Frames);

    if (surface->Role == SurfaceRole::XdgSurface && surface->Xdg != nullptr)
        XdgSurfaceCommitted(surface->Xdg);
}

void SurfaceSetBufferTransform(wl_client*, wl_resource*, int32_t) {}
void SurfaceSetBufferScale(wl_client*, wl_resource*, int32_t) {}
void SurfaceDamageBuffer(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void SurfaceOffset(wl_client*, wl_resource*, int32_t, int32_t) {}

// Requests that newer libwayland versions add are left null: they belong to
// protocol versions this compositor does not advertise. (Which is all the
// warning would complain of, built against those versions.)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const struct wl_surface_interface kSurfaceImpl = {
    .destroy = SurfaceDestroy, .attach = SurfaceAttach, .damage = SurfaceDamage, .frame = SurfaceFrame,
    .set_opaque_region = SurfaceSetRegion, .set_input_region = SurfaceSetRegion, .commit = SurfaceCommit,
    .set_buffer_transform = SurfaceSetBufferTransform, .set_buffer_scale = SurfaceSetBufferScale,
    .damage_buffer = SurfaceDamageBuffer, .offset = SurfaceOffset,
};
#pragma GCC diagnostic pop

void SurfaceResourceDestroyed(wl_resource* resource)
{
    Surface* surface = SurfaceFrom(resource);
    ForgetPendingBuffer(surface);
    DestroyCallbacks(&surface->Pending.Frames);
    DestroyCallbacks(&surface->Frames);
    ForgetSurfaceInSeat(surface);
    if (surface->Xdg != nullptr)
        surface->Xdg->Base = nullptr;
    g_surfaces.erase(std::find(g_surfaces.begin(), g_surfaces.end(), surface));
    delete surface;
}

// --- wl_region: accepted, and ignored ----------------------------------------------------

void RegionDestroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }
void RegionRect(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
const struct wl_region_interface kRegionImpl = { RegionDestroy, RegionRect, RegionRect };

// --- wl_compositor -------------------------------------------------------------------------

void CreateSurface(wl_client* client, wl_resource* compositor, uint32_t id)
{
    wl_resource* resource = wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(compositor), id);
    if (resource == nullptr) { wl_client_post_no_memory(client); return; }
    auto* surface = new Surface();
    surface->Resource = resource;
    surface->Client = client;
    surface->BufferGone.Listener.notify = PendingBufferDestroyed;
    surface->BufferGone.Owner = surface;
    wl_list_init(&surface->Pending.Frames);
    wl_list_init(&surface->Frames);
    wl_resource_set_implementation(resource, &kSurfaceImpl, surface, SurfaceResourceDestroyed);
    g_surfaces.push_back(surface);
}

void CreateRegion(wl_client* client, wl_resource*, uint32_t id)
{
    wl_resource* resource = wl_resource_create(client, &wl_region_interface, 1, id);
    if (resource == nullptr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &kRegionImpl, nullptr, nullptr);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"   // see kSurfaceImpl
const struct wl_compositor_interface kCompositorImpl = { .create_surface = CreateSurface, .create_region = CreateRegion };
#pragma GCC diagnostic pop

void BindCompositor(wl_client* client, void*, uint32_t version, uint32_t id)
{
    wl_resource* resource = wl_resource_create(client, &wl_compositor_interface, (int)version, id);
    if (resource == nullptr) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(resource, &kCompositorImpl, nullptr, nullptr);
}

} // namespace

Surface* SurfaceFrom(wl_resource* resource) { return static_cast<Surface*>(wl_resource_get_user_data(resource)); }

const std::vector<Surface*>& Surfaces() { return g_surfaces; }

const ImGui_ImplSoftRast_Texture* TextureOf(Surface* surface)
{
    return surface != nullptr && surface->HasContent ? &surface->Texture : nullptr;
}

bool CreateCompositorGlobals(wl_display* display)
{
    // Version 5: everything up to wl_surface.offset. Version 6 would oblige
    // the compositor to suggest a buffer scale.
    return wl_global_create(display, &wl_compositor_interface, 5, nullptr, BindCompositor) != nullptr;
}

} // namespace wayland
