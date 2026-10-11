// The xHCI driver. The controller is a DMA engine driven through rings of
// 16-byte TRBs in memory: the driver produces commands on the command ring
// and transfers on one ring per endpoint, and the controller produces events
// -- completions, port changes -- on the event ring, interrupting by MSI.
//
// One kernel thread per controller does everything: bringing up devices
// (a sequence of commands and control transfers, each waited for), and then
// draining the event ring, turning each HID report into input events and
// queueing the transfer for the next. The interrupt handler only
// acknowledges and wakes it.
//
// Memory the controller reads and writes is whole pages from the physical
// allocator, reached through the direct map: x86 keeps DMA coherent with
// the caches.
#include <drivers/usb/xhci.hpp>
#include <drivers/usb/hid.hpp>
#include <drivers/input.hpp>
#include <drivers/pci.hpp>
#include <arch/x86_64/clock.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <mm/memory.hpp>
#include <mm/pmm.hpp>
#include <mm/vmm.hpp>
#include <sched/events.hpp>
#include <sched/sched.hpp>
#include <klib/print.hpp>
#include <klib/string.hpp>

namespace xhci {
namespace {

constexpr u32 kMaxControllers = 8;
constexpr u64 kVectorBase = 0x40;     // one vector per controller, from here
constexpr u32 kRingSize = 256;        // TRBs per ring: one page, the last a link back
constexpr u32 kMaxDevices = 16;
constexpr u32 kMaxHid = 4;            // HID interfaces per device

// --- TRBs ----------------------------------------------------------------------------

struct Trb {
    u64 parameter;
    u32 status;
    u32 control;
};
static_assert(sizeof(Trb) == 16);

enum TrbType : u32 {
    kNormal = 1, kSetup = 2, kData = 3, kStatus = 4, kLink = 6,
    kEnableSlot = 9, kDisableSlot = 10, kAddressDevice = 11, kConfigureEndpoint = 12, kEvaluateContext = 13,
    kTransferEvent = 32, kCommandCompletion = 33, kPortStatusChange = 34,
};
constexpr u32 kCycle = 1u << 0, kToggleCycle = 1u << 1, kInterruptShort = 1u << 2, kChain = 1u << 4,
              kInterruptOnCompletion = 1u << 5, kImmediateData = 1u << 6, kDirectionIn = 1u << 16;
constexpr u32 type_of(const Trb& trb) { return (trb.control >> 10) & 0x3f; }
constexpr u32 kSuccess = 1, kShortPacket = 13;

// A page of physical memory, zeroed: its physical address, and where the
// kernel sees it.
struct Page {
    u64 physical = 0;
    template <typename T> T* as() const { return mm::phys_to_virt<T>(physical); }
    bool allocate() { physical = pmm::alloc_page_zeroed(); return physical != 0; }
};

// A ring the driver produces into: commands, or one endpoint's transfers.
struct Ring {
    Page page;
    u32 index = 0;
    bool cycle = true;

    bool init() {
        if (!page.allocate()) return false;
        // The last TRB links back to the first and flips the cycle bit.
        Trb& link = page.as<Trb>()[kRingSize - 1];
        link.parameter = page.physical;
        link.control = (kLink << 10) | kToggleCycle;
        return true;
    }

    // Write a TRB; returns its physical address. The cycle bit is written
    // last, after everything else: it is what hands the TRB over.
    u64 push(u64 parameter, u32 status, u32 control) {
        Trb* trbs = page.as<Trb>();
        Trb& trb = trbs[index];
        trb.parameter = parameter;
        trb.status = status;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        trb.control = (control & ~kCycle) | (cycle ? kCycle : 0);
        const u64 address = page.physical + index * sizeof(Trb);
        if (++index == kRingSize - 1) {
            Trb& link = trbs[kRingSize - 1];
            __atomic_thread_fence(__ATOMIC_RELEASE);
            link.control = (kLink << 10) | kToggleCycle | (cycle ? kCycle : 0);
            index = 0;
            cycle = !cycle;
        }
        return address;
    }
};

// --- devices -------------------------------------------------------------------------

struct HidInterface {
    u8   interface;
    u8   subclass, protocol;
    u8   endpoint;          // address: number, and 0x80 for IN
    u16  max_packet;
    u8   interval;          // bInterval, as the descriptor has it
    u16  report_length;     // of its report descriptor
    u8   dci;               // device context index of its endpoint
    Ring ring;
    Page buffer;
    u64  pending;           // physical address of the TRB in flight, 0 for none
    hid::Translator translator;
};

struct Device {
    bool  present;
    u8    slot, port, speed;
    Page  input, output;    // input and output device contexts
    Ring  control;
    Page  scratch;          // for control transfers' data
    u16   vendor, product;
    HidInterface hid[kMaxHid];
    u32   hid_count;
};

// The kernel's input queue, as a HID sink.
struct InputSink final : hid::Sink {
    void event(u16 type, u16 code, i32 value) override { input::post(type, code, value); }
};

class Controller;
Controller* g_by_vector[kMaxControllers];

class Controller {
public:
    explicit Controller(const pci::Device& device) : m_pci(device) {}

    bool start();
    void run();
    void interrupt();
    u32 index = 0;

private:
    // Registers.
    u32 cap32(u32 offset) const { return *reinterpret_cast<volatile u32*>(m_mmio + offset); }
    volatile u32& op(u32 offset) { return *reinterpret_cast<volatile u32*>(m_mmio + m_op + offset); }
    volatile u32& rt(u32 offset) { return *reinterpret_cast<volatile u32*>(m_mmio + m_rt + offset); }
    void write_op64(u32 offset, u64 value) { op(offset) = static_cast<u32>(value); op(offset + 4) = static_cast<u32>(value >> 32); }
    void write_rt64(u32 offset, u64 value) { rt(offset) = static_cast<u32>(value); rt(offset + 4) = static_cast<u32>(value >> 32); }
    volatile u32& portsc(u32 port) { return op(0x400 + 0x10 * (port - 1)); }
    void doorbell(u32 slot, u32 target) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        *reinterpret_cast<volatile u32*>(m_mmio + m_db + 4 * slot) = target;
    }

    // Contexts: 32 or 64 bytes each, as the controller says.
    u32* context(const Page& page, u32 index) { return reinterpret_cast<u32*>(page.as<u8>() + index * m_context_size); }

    bool take_ownership();
    void read_protocols();
    bool wait_until(u64 timeout_ms, bool (*done)(Controller*, void*), void* arg);

    // Events. The thread drains the ring; completions of the command or
    // control transfer being waited for are recorded for the waiter.
    void drain_events();
    void handle_transfer(const Trb& event);
    u32 command(u64 parameter, u32 control, u8* slot_out = nullptr);
    int control_transfer(Device& device, u8 request_type, u8 request, u16 value, u16 index, u16 length);

    // Ports and devices.
    void scan_ports();
    void attach(u32 port);
    void detach(u32 port);
    bool reset_port(u32 port);
    bool address_device(Device& device);
    bool configure(Device& device);
    bool start_hid(Device& device, HidInterface& hid);
    void queue_report(Device& device, HidInterface& hid);

    pci::Device m_pci;
    u8* m_mmio = nullptr;
    u32 m_op = 0, m_rt = 0, m_db = 0;
    u32 m_max_slots = 0, m_max_ports = 0, m_context_size = 32;
    bool m_have_msi = false;
    Page m_dcbaa, m_scratchpad_array, m_erst;
    Ring m_commands;
    Page m_events;
    u32 m_event_index = 0;
    bool m_event_cycle = true;
    u8 m_port_major[256] = {};      // 2 or 3: which USB the port speaks
    Device m_devices[kMaxDevices] = {};
    InputSink m_sink;

    // What the waiters wait for.
    u64 m_awaited = 0;              // physical address of the TRB whose completion is wanted
    bool m_completed = false;
    u32 m_completion_code = 0;
    u8 m_completion_slot = 0;
    u32 m_completion_residual = 0;
    bool m_ports_changed = false;
};

// --- bring-up ---------------------------------------------------------------------------

bool Controller::wait_until(u64 timeout_ms, bool (*done)(Controller*, void*), void* arg) {
    const u64 deadline = clock::now_ns() + timeout_ms * 1'000'000;
    while (!done(this, arg)) {
        if (clock::now_ns() >= deadline) return false;
        sched::sleep_ms(1);
    }
    return true;
}

// The firmware may be driving the controller (for a keyboard in its setup
// screens) and be told by SMIs when the OS touches it. Ask for it.
bool Controller::take_ownership() {
    u32 offset = ((cap32(0x10) >> 16) & 0xffff) * 4;
    for (u32 guard = 0; offset != 0 && guard < 64; guard++) {
        volatile u32& cap = *reinterpret_cast<volatile u32*>(m_mmio + offset);
        if ((cap & 0xff) == 1) {
            if (cap & (1u << 16)) {
                cap = cap | (1u << 24);   // the OS wants it
                const u64 deadline = clock::now_ns() + 1'000'000'000;
                while ((cap & (1u << 16)) && clock::now_ns() < deadline) sched::sleep_ms(1);
                if (cap & (1u << 16)) LOG_WARN("xhci%u: the firmware would not let go; taking it anyway", index);
            }
            cap = (cap & ~(1u << 16)) | (1u << 24);
            // Its SMIs off, their latched causes cleared.
            volatile u32& control = *reinterpret_cast<volatile u32*>(m_mmio + offset + 4);
            control = (control & 0xffff1fee) | 0xe0000000;
            return true;
        }
        const u32 next = (cap >> 8) & 0xff;
        offset = next ? offset + next * 4 : 0;
    }
    return true;
}

// Which root ports are USB 2 and which USB 3: they are brought up differently.
void Controller::read_protocols() {
    u32 offset = ((cap32(0x10) >> 16) & 0xffff) * 4;
    for (u32 guard = 0; offset != 0 && guard < 64; guard++) {
        const u32 header = *reinterpret_cast<volatile u32*>(m_mmio + offset);
        if ((header & 0xff) == 2) {
            const u8 major = static_cast<u8>(header >> 24);
            const u32 ports = *reinterpret_cast<volatile u32*>(m_mmio + offset + 8);
            const u32 first = ports & 0xff, count = (ports >> 8) & 0xff;
            for (u32 p = first; p < first + count && p < 256; p++) m_port_major[p] = major;
        }
        const u32 next = (header >> 8) & 0xff;
        offset = next ? offset + next * 4 : 0;
    }
}

bool Controller::start() {
    const pci::Bar bar = pci::bar(m_pci, 0);
    if (bar.io || bar.address == 0) return false;
    pci::enable_dma(m_pci);
    const u64 mapped = vmm::map_mmio(bar.address, bar.size ? bar.size : 0x10000);
    if (mapped == 0) return false;
    m_mmio = reinterpret_cast<u8*>(mapped);

    m_op = m_mmio[0];
    m_rt = cap32(0x18) & ~0x1fu;
    m_db = cap32(0x14) & ~0x3u;
    const u32 hcs1 = cap32(0x04), hcs2 = cap32(0x08), hcc1 = cap32(0x10);
    m_max_slots = hcs1 & 0xff;
    m_max_ports = hcs1 >> 24;
    m_context_size = (hcc1 & (1u << 2)) ? 64 : 32;
    if (m_max_slots > kMaxDevices) m_max_slots = kMaxDevices;

    take_ownership();
    read_protocols();

    // Stop it, then reset it.
    op(0x00) = op(0x00) & ~1u;
    if (!wait_until(100, [](Controller* c, void*) { return (c->op(0x04) & 1u) != 0; }, nullptr)) {
        LOG_ERROR("xhci%u: does not halt", index);
        return false;
    }
    op(0x00) = 1u << 1;
    sched::sleep_ms(1);
    if (!wait_until(1000, [](Controller* c, void*) { return !(c->op(0x00) & (1u << 1)) && !(c->op(0x04) & (1u << 11)); }, nullptr)) {
        LOG_ERROR("xhci%u: does not come out of reset", index);
        return false;
    }

    // The device context array, with scratchpad memory for the controller's
    // own use in slot 0 where it asks for some.
    if (!m_dcbaa.allocate() || !m_commands.init() || !m_events.allocate() || !m_erst.allocate()) return false;
    const u32 scratchpads = ((hcs2 >> 21) & 0x1f) << 5 | (hcs2 >> 27);
    if (scratchpads) {
        if (!m_scratchpad_array.allocate() || scratchpads > 512) return false;
        for (u32 i = 0; i < scratchpads; i++) {
            Page page;
            if (!page.allocate()) return false;
            m_scratchpad_array.as<u64>()[i] = page.physical;
        }
        m_dcbaa.as<u64>()[0] = m_scratchpad_array.physical;
    }
    op(0x38) = m_max_slots;                              // CONFIG: slots enabled
    write_op64(0x30, m_dcbaa.physical);                  // DCBAAP
    write_op64(0x18, m_commands.page.physical | 1);      // CRCR, ring cycle state 1

    // One event ring segment, for interrupter 0.
    u64* erst = m_erst.as<u64>();
    erst[0] = m_events.physical;
    erst[1] = kRingSize;
    rt(0x28) = 1;                                        // ERSTSZ
    write_rt64(0x38, m_events.physical);                 // ERDP
    write_rt64(0x30, m_erst.physical);                   // ERSTBA
    rt(0x24) = 160;                                      // IMOD: at most an interrupt per 40 us
    rt(0x20) = (1u << 1) | 1u;                           // IMAN: enabled, pending cleared

    // Interrupts by message; without them, the thread looks every tick.
    const u8 vector = static_cast<u8>(kVectorBase + index);
    g_by_vector[index] = this;
    m_have_msi = pci::enable_msi(m_pci, vector, percpu::current()->lapic_id);

    op(0x00) = 1u | (1u << 2);                           // run, with interrupts
    if (!wait_until(100, [](Controller* c, void*) { return (c->op(0x04) & 1u) == 0; }, nullptr)) {
        LOG_ERROR("xhci%u: does not start", index);
        return false;
    }
    // Power to every port that needs switching on.
    for (u32 port = 1; port <= m_max_ports; port++)
        if (!(portsc(port) & (1u << 9))) portsc(port) = 1u << 9;

    LOG_INFO("xhci%u: %02x:%02x.%u, %u ports, %u slots, %u-byte contexts, %u scratchpad pages, %s",
             index, m_pci.bus, m_pci.slot, m_pci.function, m_max_ports, m_max_slots, m_context_size, scratchpads,
             m_have_msi ? "msi" : "polled");
    return true;
}

void Controller::interrupt() {
    // Acknowledge: the interrupter's pending bit and the controller's.
    rt(0x20) = rt(0x20) | 1u;
    op(0x04) = 1u << 3;
    events::notify();
}

// --- events -----------------------------------------------------------------------------

void Controller::drain_events() {
    Trb* events = m_events.as<Trb>();
    bool any = false;
    for (;;) {
        const Trb event = events[m_event_index];
        if (((event.control & kCycle) != 0) != m_event_cycle) break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        any = true;
        switch (type_of(event)) {
        case kCommandCompletion:
            if (event.parameter == m_awaited) {
                m_completed = true;
                m_completion_code = event.status >> 24;
                m_completion_slot = static_cast<u8>(event.control >> 24);
            }
            break;
        case kTransferEvent:
            handle_transfer(event);
            break;
        case kPortStatusChange:
            m_ports_changed = true;
            break;
        default:
            break;
        }
        if (++m_event_index == kRingSize) {
            m_event_index = 0;
            m_event_cycle = !m_event_cycle;
        }
    }
    if (any) write_rt64(0x38, (m_events.physical + m_event_index * sizeof(Trb)) | (1u << 3));
}

void Controller::handle_transfer(const Trb& event) {
    const u32 code = event.status >> 24;
    if (event.parameter == m_awaited) {
        m_completed = true;
        m_completion_code = code;
        m_completion_residual = event.status & 0xffffff;
        return;
    }
    // A HID report.
    const u8 slot = static_cast<u8>(event.control >> 24);
    for (Device& device : m_devices) {
        if (!device.present || device.slot != slot) continue;
        for (u32 i = 0; i < device.hid_count; i++) {
            HidInterface& hid = device.hid[i];
            if (hid.pending != event.parameter) continue;
            hid.pending = 0;
            if (code == kSuccess || code == kShortPacket) {
                const u32 residual = event.status & 0xffffff;
                const u32 length = hid.max_packet > residual ? hid.max_packet - residual : 0;
                hid.translator.report(hid.buffer.as<u8>(), length, m_sink);
                queue_report(device, hid);
            } else {
                LOG_WARN("xhci%u: slot %u endpoint %#x: transfer failed (completion code %u); no more reports from it",
                         index, slot, hid.endpoint, code);
            }
            return;
        }
    }
}

// Submit a command and wait for it: its completion code, 0 if it never
// completed. The slot it names, for Enable Slot.
u32 Controller::command(u64 parameter, u32 control, u8* slot_out) {
    m_completed = false;
    m_awaited = m_commands.push(parameter, 0, control);
    doorbell(0, 0);
    const u64 deadline = clock::now_ns() + 1'000'000'000;
    while (!m_completed && clock::now_ns() < deadline) {
        const u64 snapshot = events::snapshot();
        drain_events();
        if (!m_completed) events::wait(snapshot, clock::now_ns() + (m_have_msi ? 20'000'000 : 2'000'000));
    }
    m_awaited = 0;
    if (!m_completed) return 0;
    if (slot_out) *slot_out = m_completion_slot;
    return m_completion_code;
}

// A control transfer on endpoint 0: setup, data in or out of the device's
// scratch page, status. Returns the bytes transferred, or -1.
int Controller::control_transfer(Device& device, u8 request_type, u8 request, u16 value, u16 index_value, u16 length) {
    const bool in = request_type & 0x80;
    const u64 setup = request_type | (static_cast<u64>(request) << 8) | (static_cast<u64>(value) << 16) |
                      (static_cast<u64>(index_value) << 32) | (static_cast<u64>(length) << 48);
    const u32 transfer_type = length == 0 ? 0 : (in ? 3 : 2);
    device.control.push(setup, 8, (kSetup << 10) | kImmediateData | (transfer_type << 16));
    if (length) device.control.push(device.scratch.physical, length, (kData << 10) | (in ? kDirectionIn : 0));
    m_completed = false;
    m_awaited = device.control.push(0, 0, (kStatus << 10) | kInterruptOnCompletion | (in && length ? 0 : kDirectionIn));
    doorbell(device.slot, 1);
    const u64 deadline = clock::now_ns() + 1'000'000'000;
    while (!m_completed && clock::now_ns() < deadline) {
        const u64 snapshot = events::snapshot();
        drain_events();
        if (!m_completed) events::wait(snapshot, clock::now_ns() + (m_have_msi ? 20'000'000 : 2'000'000));
    }
    m_awaited = 0;
    if (!m_completed || (m_completion_code != kSuccess && m_completion_code != kShortPacket)) return -1;
    return length;
}

// --- ports ------------------------------------------------------------------------------

constexpr u32 kPortConnected = 1u << 0, kPortEnabled = 1u << 1, kPortReset = 1u << 4, kPortPower = 1u << 9,
              kPortChanges = 0x7fu << 17, kPortResetChange = 1u << 21, kPortWarmReset = 1u << 31;

bool Controller::reset_port(u32 port) {
    const u32 status = portsc(port);
    // USB 3 ports enable themselves when the link trains; USB 2 ones are
    // reset into it.
    if (m_port_major[port] == 3 && (status & kPortEnabled)) return true;
    // Write: keep the power, clear the change bits, never write 1 to
    // "enabled" (that disables the port).
    const u32 reset = m_port_major[port] == 3 ? kPortWarmReset : kPortReset;
    portsc(port) = (status & ~(kPortEnabled | kPortChanges)) | kPortPower | reset;
    struct Arg { u32 port; } arg{ port };
    const bool done = wait_until(500, [](Controller* c, void* a) {
        return (c->portsc(static_cast<Arg*>(a)->port) & kPortResetChange) != 0;
    }, &arg);
    portsc(port) = (portsc(port) & ~kPortEnabled & ~kPortChanges) | kPortPower | kPortResetChange | (1u << 19);
    sched::sleep_ms(10);   // recovery time after reset, before the device need answer
    return done && (portsc(port) & kPortEnabled);
}

void Controller::scan_ports() {
    for (u32 port = 1; port <= m_max_ports; port++) {
        const u32 status = portsc(port);
        // Acknowledge whatever changed.
        if (status & kPortChanges) portsc(port) = (status & ~kPortEnabled) | kPortPower;
        bool known = false;
        for (Device& device : m_devices) if (device.present && device.port == port) known = true;
        if ((status & kPortConnected) && !known) attach(port);
        else if (!(status & kPortConnected) && known) detach(port);
    }
}

void Controller::detach(u32 port) {
    for (Device& device : m_devices) {
        if (!device.present || device.port != port) continue;
        LOG_INFO("xhci%u: port %u: %04x:%04x disconnected", index, port, device.vendor, device.product);
        command(0, (kDisableSlot << 10) | (static_cast<u32>(device.slot) << 24));
        m_dcbaa.as<u64>()[device.slot] = 0;
        // The pages are leaked: few devices come and go, and freeing pages
        // the controller may still be finishing with is worse.
        device.present = false;
    }
}

bool Controller::address_device(Device& device) {
    if (!device.input.allocate() || !device.output.allocate() || !device.control.init() || !device.scratch.allocate()) return false;
    m_dcbaa.as<u64>()[device.slot] = device.output.physical;

    // The default packet size for endpoint 0, until the device says.
    u32 max_packet = device.speed == 3 ? 64 : (device.speed >= 4 ? 512 : 8);
    u32* control = context(device.input, 0);
    control[1] = 0b11;                                                   // add the slot and endpoint 0
    u32* slot = context(device.input, 1);
    slot[0] = (1u << 27) | (static_cast<u32>(device.speed) << 20);      // one context entry; the speed
    slot[1] = static_cast<u32>(device.port) << 16;                       // its root hub port
    u32* ep0 = context(device.input, 2);
    ep0[1] = (3u << 1) | (4u << 3) | (max_packet << 16);                 // 3 retries, control, packet size
    ep0[2] = static_cast<u32>(device.control.page.physical) | 1;         // dequeue pointer, cycle 1
    ep0[3] = static_cast<u32>(device.control.page.physical >> 32);
    ep0[4] = 8;                                                          // average TRB length
    if (command(device.input.physical, (kAddressDevice << 10) | (static_cast<u32>(device.slot) << 24)) != kSuccess) return false;

    // The first eight bytes of the device descriptor say the real packet size.
    if (control_transfer(device, 0x80, 6, 0x0100, 0, 8) < 0) return false;
    const u8* descriptor = device.scratch.as<u8>();
    const u32 actual = device.speed >= 4 ? (1u << descriptor[7]) : descriptor[7];
    if (actual != max_packet && actual >= 8) {
        control[0] = 0;
        control[1] = 0b10;                                               // endpoint 0 only
        ep0[1] = (ep0[1] & 0xffff) | (actual << 16);
        if (command(device.input.physical, (kEvaluateContext << 10) | (static_cast<u32>(device.slot) << 24)) != kSuccess)
            return false;
    }
    if (control_transfer(device, 0x80, 6, 0x0100, 0, 18) < 0) return false;
    memcpy(&device.vendor, descriptor + 8, 2);
    memcpy(&device.product, descriptor + 10, 2);
    return true;
}

// Read the configuration, choose it, and find the HID interfaces in it.
bool Controller::configure(Device& device) {
    if (control_transfer(device, 0x80, 6, 0x0200, 0, 9) < 0) return false;
    u16 total;
    memcpy(&total, device.scratch.as<u8>() + 2, 2);
    // Input devices' configurations are a few hundred bytes; more is read
    // as far as this, and the rest ignored.
    u8 config[1024];
    if (total > sizeof(config)) total = sizeof(config);
    if (control_transfer(device, 0x80, 6, 0x0200, 0, total) < 0) return false;
    // A copy: the scratch page is reused by the requests that follow.
    memcpy(config, device.scratch.as<u8>(), total);
    const u8 configuration_value = config[5];

    HidInterface* current = nullptr;
    for (u32 i = 0; i + 2 <= total && config[i] >= 2; i += config[i]) {
        const u8* d = config + i;
        if (i + d[0] > total) break;
        if (d[1] == 4 && d[0] >= 9) {                 // interface
            current = nullptr;
            if (d[5] == 3 && device.hid_count < kMaxHid) {   // class HID
                current = &device.hid[device.hid_count];
                *current = HidInterface();
                current->interface = d[2];
                current->subclass = d[6];
                current->protocol = d[7];
            } else if (d[5] == 9) {
                LOG_WARN("xhci%u: port %u: a hub -- not supported yet; what is behind it will not work", index, device.port);
            }
        } else if (d[1] == 0x21 && current && d[0] >= 9) { // HID descriptor
            memcpy(&current->report_length, d + 7, 2);
        } else if (d[1] == 5 && current && d[0] >= 7 && current->endpoint == 0) {   // endpoint
            if ((d[2] & 0x80) && (d[3] & 3) == 3) {  // interrupt IN
                current->endpoint = d[2];
                memcpy(&current->max_packet, d + 4, 2);
                current->max_packet &= 0x7ff;
                current->interval = d[6];
                device.hid_count++;
                current = nullptr;
            }
        }
    }
    if (control_transfer(device, 0x00, 9, configuration_value, 0, 0) < 0) return false;   // SET_CONFIGURATION
    return true;
}

// Make one HID interface's reports understood, its endpoint configured, and
// its first transfer queued.
bool Controller::start_hid(Device& device, HidInterface& hid) {
    const bool boot = hid.subclass == 1 && (hid.protocol == 1 || hid.protocol == 2);
    hid::Descriptor descriptor;
    bool described = false;
    if (hid.report_length > 0 && hid.report_length <= 4096 &&
        control_transfer(device, 0x81, 6, 0x2200, hid.interface, hid.report_length) >= 0)
        described = hid::parse(device.scratch.as<u8>(), hid.report_length, descriptor);
    if (described) {
        hid.translator.use(descriptor);
        control_transfer(device, 0x21, 0x0b, 1, hid.interface, 0);     // SET_PROTOCOL: report
    } else if (boot) {
        // The boot protocol's reports are fixed, and every boot device has them.
        control_transfer(device, 0x21, 0x0b, 0, hid.interface, 0);     // SET_PROTOCOL: boot
        if (hid.protocol == 1) hid.translator.boot_keyboard(); else hid.translator.boot_mouse();
    } else {
        return false;
    }
    control_transfer(device, 0x21, 0x0a, 0, hid.interface, 0);         // SET_IDLE: report changes only

    if (!hid.ring.init() || !hid.buffer.allocate()) return false;
    hid.dci = static_cast<u8>((hid.endpoint & 0xf) * 2 + 1);
    // The interval in the controller's units: 2^n of 125 us.
    u32 interval;
    if (device.speed >= 3) {
        interval = hid.interval >= 1 && hid.interval <= 16 ? hid.interval - 1u : 3u;
    } else {
        u32 frames = hid.interval ? hid.interval * 8u : 8u;
        interval = 0;
        while ((2u << interval) <= frames && interval < 10) interval++;
        if (interval < 3) interval = 3;
    }
    u32* control = context(device.input, 0);
    control[0] = 0;
    control[1] = 1u | (1u << hid.dci);
    u32* slot = context(device.input, 1);
    const u32 entries = (slot[0] >> 27) > hid.dci ? (slot[0] >> 27) : hid.dci;
    slot[0] = (slot[0] & 0x07ffffff) | (entries << 27);
    u32* ep = context(device.input, hid.dci + 1);
    memset(ep, 0, m_context_size);
    ep[0] = interval << 16;
    ep[1] = (3u << 1) | (7u << 3) | (static_cast<u32>(hid.max_packet) << 16);   // 3 retries, interrupt IN
    ep[2] = static_cast<u32>(hid.ring.page.physical) | 1;
    ep[3] = static_cast<u32>(hid.ring.page.physical >> 32);
    ep[4] = hid.max_packet | (static_cast<u32>(hid.max_packet) << 16);          // average TRB length, max ESIT payload
    if (command(device.input.physical, (kConfigureEndpoint << 10) | (static_cast<u32>(device.slot) << 24)) != kSuccess)
        return false;
    queue_report(device, hid);
    return true;
}

void Controller::queue_report(Device& device, HidInterface& hid) {
    hid.pending = hid.ring.push(hid.buffer.physical, hid.max_packet, (kNormal << 10) | kInterruptOnCompletion | kInterruptShort);
    doorbell(device.slot, hid.dci);
}

void Controller::attach(u32 port) {
    Device* device = nullptr;
    for (Device& candidate : m_devices) if (!candidate.present) { device = &candidate; break; }
    if (device == nullptr) return;
    if (!reset_port(port)) {
        LOG_WARN("xhci%u: port %u: a device, but the port would not enable", index, port);
        return;
    }
    *device = Device();
    device->port = static_cast<u8>(port);
    device->speed = static_cast<u8>((portsc(port) >> 10) & 0xf);
    u8 slot = 0;
    if (command(0, kEnableSlot << 10, &slot) != kSuccess || slot == 0 || slot > m_max_slots) {
        LOG_WARN("xhci%u: port %u: no slot for the device", index, port);
        return;
    }
    device->slot = slot;
    device->present = true;
    if (!address_device(*device) || !configure(*device)) {
        LOG_WARN("xhci%u: port %u: the device did not answer its setup", index, port);
        return;
    }
    static const char* const kSpeeds[] = { "?", "full", "low", "high", "super", "super+" };
    u32 started = 0;
    for (u32 i = 0; i < device->hid_count; i++) {
        HidInterface& hid = device->hid[i];
        if (!start_hid(*device, hid)) continue;
        started++;
        LOG_INFO("xhci%u: port %u: %04x:%04x, %s speed: interface %u is a %s%s", index, port, device->vendor,
                 device->product, kSpeeds[device->speed < 6 ? device->speed : 0], hid.interface,
                 hid.translator.is_keyboard() ? "keyboard" : hid.translator.is_pointer() ? "pointer" : "HID device",
                 hid.report_length && hid.subclass != 1 ? "" : (hid.subclass == 1 ? " (boot capable)" : ""));
    }
    if (started == 0)
        LOG_INFO("xhci%u: port %u: %04x:%04x, %s speed: nothing here this kernel uses", index, port, device->vendor,
                 device->product, kSpeeds[device->speed < 6 ? device->speed : 0]);
}

void Controller::run() {
    // Devices present at boot announce themselves with port changes too,
    // but a scan does not depend on that.
    sched::sleep_ms(50);
    scan_ports();
    for (;;) {
        const u64 snapshot = events::snapshot();
        drain_events();
        if (m_ports_changed) {
            m_ports_changed = false;
            sched::sleep_ms(50);   // let a plugged device settle
            scan_ports();
            continue;
        }
        // Woken by our interrupt (or anything else); without MSI, every
        // couple of ticks regardless.
        events::wait(snapshot, m_have_msi ? 0 : clock::now_ns() + 10'000'000);
    }
}

Controller* g_controllers[kMaxControllers];
u32 g_count = 0;

template <u32 N>
void on_interrupt() {
    if (g_by_vector[N] != nullptr) g_by_vector[N]->interrupt();
}

constexpr interrupts::Handler kHandlers[kMaxControllers] = {
    on_interrupt<0>, on_interrupt<1>, on_interrupt<2>, on_interrupt<3>,
    on_interrupt<4>, on_interrupt<5>, on_interrupt<6>, on_interrupt<7>,
};

void controller_main(void* argument) {
    auto* controller = static_cast<Controller*>(argument);
    if (!controller->start()) {
        LOG_ERROR("xhci%u: could not be started", controller->index);
        return;
    }
    controller->run();
}

} // namespace

void init() {
    for (u32 i = 0; g_count < kMaxControllers; i++) {
        const pci::Device* device = pci::find_class(0x0c, 0x03, 0x30, i);
        if (device == nullptr) break;
        auto* controller = new Controller(*device);
        controller->index = g_count;
        interrupts::set_handler(kVectorBase + g_count, kHandlers[g_count]);
        g_controllers[g_count++] = controller;
        // Bringing a controller up waits on hardware: in a thread of its own.
        sched::spawn("xhci", controller_main, controller);
    }
    if (g_count == 0) LOG_INFO("xhci: no USB 3 host controllers");
}

} // namespace xhci
