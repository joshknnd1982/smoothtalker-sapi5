#include "st_engine.hpp"

#include "unicorn_dyn.hpp"
#include "../common/st_log.hpp"
#include "../common/st_paths.hpp"

#include <unicorn/x86.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace st {

namespace {

//: 1 MB of conventional memory plus the HMA wrap area.  The image is written
//: at linear 0, so every far pointer captured in the snapshot stays valid.
constexpr uint64_t MEM_SIZE = 0x110000;
constexpr size_t DUMP_HDR = 16;
constexpr uint16_t STACK_SEG = 0x4000;
constexpr uint16_t STACK_TOP = 0xFFF0;
//: A far return to this address is how we learn the ROM finished: it is
//: pushed as the return address and never mapped to any code.
constexpr uint16_t SENTINEL_SEG = 0x5FFF;
constexpr uint64_t SENTINEL = static_cast<uint64_t>(SENTINEL_SEG) * 16;

constexpr uint16_t FN_INIT = 0x02;
constexpr uint16_t FN_SPEAK = 0x07;

//: Settings block, relative to the text buffer:
//:   +0x200 gender  +0x202 tone  +0x204 volume  +0x206 pitch  +0x208 speed
//: Values above 9 clamp to 9 inside the ROM.  Applied by an AL=2 call.
constexpr uint16_t PARAM_OFF = 0x200;

constexpr uint32_t SB_BASE = 0x220;
constexpr int SB_IRQ = 7;

constexpr size_t MAX_INSNS = 50000000;
constexpr int MAX_BLOCKS = 4096;
//: Safety net only; a correct length byte ends the utterance cleanly.
constexpr int SILENCE_LIMIT = 16;

constexpr int DEFAULT_RATE_HZ = 11025;

[[nodiscard]] std::string narrow(const std::wstring& s)
{
    if (s.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

//: Enough Sound Blaster DSP and 8237 DMA to satisfy BLASTER.DRV and capture
//: what it plays.  The ROM talks to real hardware; this is the hardware.
struct sound_blaster {
    std::deque<uint8_t> readfifo;
    std::vector<uint8_t> pending;
    int time_constant = -1;
    uint32_t dma_addr = 0;
    uint32_t dma_page = 0;
    uint32_t dma_count = 0;
    int flipflop = 0;
    int blocks = 0;
    bool irq_pending = false;
    bool has_pending_dma = false;
    uint32_t pending_phys = 0;
    uint32_t pending_len = 0;
    int silent_run = 0;
    bool finished = false;

    [[nodiscard]] int sample_rate() const
    {
        if (time_constant < 0) {
            return 0;
        }
        const int divisor = 256 - time_constant;
        if (divisor <= 0) {
            return 0;
        }
        return static_cast<int>(std::llround(1000000.0 / divisor));
    }

    [[nodiscard]] uint32_t read(uint32_t port)
    {
        if (port == SB_BASE + 0x0C) {
            return 0x00;  // write buffer always ready
        }
        if (port == SB_BASE + 0x0E) {
            return readfifo.empty() ? 0x00 : 0x80;
        }
        if (port == SB_BASE + 0x0A) {
            if (readfifo.empty()) {
                return 0xFF;
            }
            const uint8_t v = readfifo.front();
            readfifo.pop_front();
            return v;
        }
        return 0xFF;
    }

    void write(uint32_t port, uint32_t value)
    {
        const uint8_t v = static_cast<uint8_t>(value & 0xFF);
        if (port == SB_BASE + 0x06) {
            if (v & 1) {
                readfifo.clear();
                pending.clear();
            } else {
                readfifo.push_back(0xAA);  // reset acknowledged
            }
        } else if (port == SB_BASE + 0x0C) {
            dsp(v);
        } else if (port == 0x02) {
            dma_addr = flipflop ? ((dma_addr & 0x00FF) | (v << 8))
                                : ((dma_addr & 0xFF00) | v);
            flipflop ^= 1;
        } else if (port == 0x03) {
            dma_count = flipflop ? ((dma_count & 0x00FF) | (v << 8))
                                 : ((dma_count & 0xFF00) | v);
            flipflop ^= 1;
        } else if (port == 0x0C) {
            flipflop = 0;
        } else if (port == 0x81 || port == 0x82 || port == 0x83 ||
                   port == 0x87) {
            dma_page = v;
        }
    }

    //: Arming a transfer raises has_pending_dma, which is the caller's cue to
    //: stop the emulator and hand the block over.
    void dsp(uint8_t v)
    {
        if (!pending.empty()) {
            pending.push_back(v);
            const uint8_t cmd = pending[0];
            if (cmd == 0x40 && pending.size() == 2) {
                time_constant = pending[1];
                pending.clear();
            } else if ((cmd == 0x14 || cmd == 0x1C || cmd == 0x48) &&
                       pending.size() == 3) {
                const uint32_t length =
                    static_cast<uint32_t>((pending[2] << 8) | pending[1]) + 1;
                pending.clear();
                if (cmd != 0x48) {  // 0x48 only sets the block size
                    arm(length);
                }
            }
            return;
        }
        if (v == 0x40 || v == 0x14 || v == 0x1C || v == 0x48) {
            pending.assign(1, v);
        } else if (v == 0xE1) {  // DSP version -> 2.1
            readfifo.push_back(2);
            readfifo.push_back(1);
        }
    }

    void arm(uint32_t length)
    {
        pending_phys = (dma_page << 16) | dma_addr;
        pending_len = length;
        has_pending_dma = true;
        irq_pending = true;
    }
};

}

// ---------------------------------------------------------------------------

class engine::impl
{
public:
    const unicorn_api* u = nullptr;
    std::vector<uint8_t> image;
    uint16_t res_seg = 0;
    uint16_t res_bx = 0;
    uint16_t ent_off = 0;
    uint16_t ent_seg = 0;
    uint16_t buf_off = 0;

    uc_engine* uc = nullptr;
    bool dirty = false;
    bool just_rebuilt = false;
    bool have_params = false;
    engine_params params;

    // Per-run state, replaced by each speak()/configure().
    sound_blaster sb;
    uint64_t ticks = 0;
    uint64_t blockcount = 0;
    bool cancelled = false;
    bool faulted = false;
    std::string fault;
    const engine::block_fn* on_block = nullptr;
    const engine::cancel_fn* should_cancel = nullptr;
    int native_rate = DEFAULT_RATE_HZ;

    ~impl() { drop(); }

    void drop()
    {
        if (uc && u) {
            u->close(uc);
        }
        uc = nullptr;
        dirty = false;
    }

    // -- register access ----------------------------------------------------
    // Unicorn reads and writes exactly the register's own width, so the
    // 16-bit registers must be handed 16-bit storage and EFLAGS 32-bit.
    [[nodiscard]] uint16_t r16(int reg) const
    {
        uint16_t v = 0;
        u->reg_read(uc, reg, &v);
        return v;
    }

    void w16(int reg, uint16_t v) { u->reg_write(uc, reg, &v); }

    [[nodiscard]] uint32_t eflags() const
    {
        uint32_t v = 0;
        u->reg_read(uc, UC_X86_REG_EFLAGS, &v);
        return v;
    }

    void set_eflags(uint32_t v) { u->reg_write(uc, UC_X86_REG_EFLAGS, &v); }

    void push(uint16_t value)
    {
        const uint16_t sp = static_cast<uint16_t>(r16(UC_X86_REG_SP) - 2);
        const uint16_t ss = r16(UC_X86_REG_SS);
        u->mem_write(uc, static_cast<uint64_t>(ss) * 16 + sp, &value, 2);
        w16(UC_X86_REG_SP, sp);
    }

    // -- hooks --------------------------------------------------------------
    static uint32_t hook_in(uc_engine*, uint32_t port, int, void* user)
    {
        return static_cast<impl*>(user)->sb.read(port);
    }

    static void hook_out(uc_engine* uc, uint32_t port, int, uint32_t value,
                         void* user)
    {
        auto* self = static_cast<impl*>(user);
        self->sb.write(port, value);
        if (self->sb.has_pending_dma) {
            // A transfer was just armed.  Stop here so the block can be
            // handed to the caller and the completion IRQ delivered by hand;
            // there is no timer driving this machine.
            self->u->emu_stop(uc);
        }
    }

    static void hook_intr(uc_engine* uc, uint32_t intno, void* user)
    {
        auto* self = static_cast<impl*>(user);
        const uint16_t ax = self->r16(UC_X86_REG_AX);
        const uint8_t ah = static_cast<uint8_t>((ax >> 8) & 0xFF);
        const uint8_t al = static_cast<uint8_t>(ax & 0xFF);

        if (intno == 0x21) {
            if (ah == 0x35) {  // get interrupt vector
                uint16_t vec[2] = {0, 0};
                self->u->mem_read(uc, static_cast<uint64_t>(al) * 4, vec, 4);
                self->w16(UC_X86_REG_ES, vec[1]);
                self->w16(UC_X86_REG_BX, vec[0]);
            } else if (ah == 0x25) {  // set interrupt vector
                const uint16_t vec[2] = {self->r16(UC_X86_REG_DX),
                                         self->r16(UC_X86_REG_DS)};
                self->u->mem_write(uc, static_cast<uint64_t>(al) * 4, vec, 4);
            } else if (ah == 0x30) {  // DOS version -> 5.0
                self->w16(UC_X86_REG_AX, 0x0005);
            } else if (ah == 0x2A || ah == 0x2C) {  // date / time
                self->w16(UC_X86_REG_CX, 0);
                self->w16(UC_X86_REG_DX, 0);
            }
            self->set_eflags(self->eflags() & ~0x01u);  // clear CF: success
        } else if (intno == 0x16) {  // keyboard: there is never a key
            if (ah == 0x01 || ah == 0x11) {
                self->set_eflags(self->eflags() | 0x40u);  // ZF: buffer empty
            }
            self->w16(UC_X86_REG_AX, 0);
        } else if (intno == 0x1A && ah == 0x00) {  // read tick count
            self->w16(UC_X86_REG_CX,
                      static_cast<uint16_t>((self->ticks >> 16) & 0xFFFF));
            self->w16(UC_X86_REG_DX,
                      static_cast<uint16_t>(self->ticks & 0xFFFF));
            self->ticks++;
        }
    }

    static void hook_block(uc_engine* uc, uint64_t, uint32_t, void* user)
    {
        auto* self = static_cast<impl*>(user);
        self->blockcount++;
        // Advancing the BIOS tick counter every 2000 basic blocks is enough
        // for the ROM's delay loops to make progress without the cost of
        // touching memory on every block.
        if (self->blockcount % 2000) {
            return;
        }
        self->ticks++;
        const uint32_t t = static_cast<uint32_t>(self->ticks & 0xFFFFFFFF);
        self->u->mem_write(uc, 0x46C, &t, 4);
        if (self->should_cancel && *self->should_cancel &&
            (*self->should_cancel)()) {
            self->cancelled = true;
            self->u->emu_stop(uc);
        }
    }

    // -- emulation ----------------------------------------------------------
    [[nodiscard]] bool ensure()
    {
        just_rebuilt = false;
        if (uc && !dirty) {
            return true;
        }
        // The ROM is a TSR: the original software called it over and over
        // without reloading anything, so keeping one instance alive is what
        // it was built for -- and it avoids re-mapping 1 MB and rewriting
        // 640 KB on every utterance.  A rebuild only happens after a call
        // that did not return cleanly.
        drop();
        just_rebuilt = true;
        faulted = false;
        fault.clear();

        uc_err e = u->open(UC_ARCH_X86, UC_MODE_16, &uc);
        if (e != UC_ERR_OK) {
            uc = nullptr;
            faulted = true;
            fault = std::string("uc_open: ") + u->strerror(e);
            ST_ERROR("engine: %s", fault.c_str());
            return false;
        }
        e = u->mem_map(uc, 0, MEM_SIZE, UC_PROT_ALL);
        if (e != UC_ERR_OK) {
            faulted = true;
            fault = std::string("uc_mem_map: ") + u->strerror(e);
            ST_ERROR("engine: %s", fault.c_str());
            drop();
            return false;
        }
        e = u->mem_write(uc, 0, image.data(), image.size());
        if (e != UC_ERR_OK) {
            faulted = true;
            fault = std::string("uc_mem_write(image): ") + u->strerror(e);
            ST_ERROR("engine: %s", fault.c_str());
            drop();
            return false;
        }

        uc_hook h = 0;
        u->hook_add(uc, &h, UC_HOOK_INSN, reinterpret_cast<void*>(&hook_in),
                    this, 1, 0, UC_X86_INS_IN);
        u->hook_add(uc, &h, UC_HOOK_INSN, reinterpret_cast<void*>(&hook_out),
                    this, 1, 0, UC_X86_INS_OUT);
        u->hook_add(uc, &h, UC_HOOK_INTR, reinterpret_cast<void*>(&hook_intr),
                    this, 1, 0);
        u->hook_add(uc, &h, UC_HOOK_BLOCK, reinterpret_cast<void*>(&hook_block),
                    this, 1, 0);

        ST_DEBUG("engine: emulator rebuilt (image %zu bytes at linear 0)",
                 image.size());

        // A rebuilt image is back at the ROM's own defaults, so anything we
        // were asked for has to be re-applied.
        if (have_params) {
            apply(params);
        }
        return !faulted;
    }

    void begin_run(const engine::block_fn* blk, const engine::cancel_fn* cancel)
    {
        sb = sound_blaster();
        ticks = 0;
        blockcount = 0;
        cancelled = false;
        on_block = blk;
        should_cancel = cancel;
    }

    void setup_regs(uint16_t func)
    {
        w16(UC_X86_REG_SS, STACK_SEG);
        w16(UC_X86_REG_SP, STACK_TOP);
        w16(UC_X86_REG_DS, res_seg);
        w16(UC_X86_REG_ES, res_seg);
        w16(UC_X86_REG_SI, buf_off);
        w16(UC_X86_REG_DI, buf_off);
        w16(UC_X86_REG_BX, res_bx);
        w16(UC_X86_REG_CX, 0);
        w16(UC_X86_REG_DX, 0);
        w16(UC_X86_REG_BP, 0);
        w16(UC_X86_REG_AX, func);
        w16(UC_X86_REG_CS, ent_seg);
        w16(UC_X86_REG_IP, ent_off);
        push(SENTINEL_SEG);
        push(0x0000);
    }

    void apply(const engine_params& p)
    {
        begin_run(nullptr, nullptr);
        const uint16_t words[5] = {
            static_cast<uint16_t>(p.gender), static_cast<uint16_t>(p.tone),
            static_cast<uint16_t>(p.volume), static_cast<uint16_t>(p.pitch),
            static_cast<uint16_t>(p.speed)};
        u->mem_write(uc,
                     static_cast<uint64_t>(res_seg) * 16 + buf_off + PARAM_OFF,
                     words, sizeof(words));
        setup_regs(FN_INIT);
        run_loop();
    }

    //: Hand the armed DMA block to the caller.  Returns false if the caller
    //: asked to stop.
    [[nodiscard]] bool commit()
    {
        if (!sb.has_pending_dma) {
            return true;
        }
        sb.has_pending_dma = false;
        const uint32_t phys = sb.pending_phys;
        const uint32_t len = sb.pending_len;

        std::vector<uint8_t> data(len);
        if (len) {
            const uc_err e = u->mem_read(uc, phys, data.data(), len);
            if (e != UC_ERR_OK) {
                faulted = true;
                fault = std::string("uc_mem_read(dma): ") + u->strerror(e);
                ST_ERROR("engine: %s", fault.c_str());
                return false;
            }
        }
        sb.blocks++;
        const int rate = sb.sample_rate() ? sb.sample_rate() : DEFAULT_RATE_HZ;
        native_rate = rate;

        ST_TRACE("engine: DMA block %d, %u bytes at 0x%05X, %d Hz", sb.blocks,
                 len, phys, rate);

        bool keep_going = true;
        if (on_block && *on_block) {
            keep_going = (*on_block)(data.data(), data.size(), rate);
        }

        // The ROM keeps handing over blocks of silence after the last real
        // audio; a run of them means the utterance is over.
        if (!data.empty()) {
            const auto mm = std::minmax_element(data.begin(), data.end());
            if (static_cast<int>(*mm.second) - static_cast<int>(*mm.first) < 4) {
                if (++sb.silent_run >= SILENCE_LIMIT) {
                    sb.finished = true;
                }
            } else {
                sb.silent_run = 0;
            }
        }
        return keep_going;
    }

    //: Deliver the Sound Blaster completion IRQ the way the PIC would.
    [[nodiscard]] bool vector_irq(uint64_t* pc)
    {
        const int vec = 0x08 + SB_IRQ;
        uint16_t v[2] = {0, 0};
        u->mem_read(uc, static_cast<uint64_t>(vec) * 4, v, 4);
        const uint16_t off = v[0];
        const uint16_t seg = v[1];
        if (!seg && !off) {
            return false;
        }
        push(static_cast<uint16_t>(eflags() & 0xFFFF));
        push(r16(UC_X86_REG_CS));
        push(r16(UC_X86_REG_IP));
        w16(UC_X86_REG_CS, seg);
        w16(UC_X86_REG_IP, off);
        *pc = static_cast<uint64_t>(seg) * 16 + off;
        return true;
    }

    void run_loop()
    {
        bool clean = false;
        uint64_t pc = static_cast<uint64_t>(ent_seg) * 16 + ent_off;

        for (int i = 0; i < MAX_BLOCKS + 64; ++i) {
            const uc_err e = u->emu_start(uc, pc, SENTINEL, 0, MAX_INSNS);
            if (e != UC_ERR_OK) {
                faulted = true;
                fault = std::string("emulation fault: ") + u->strerror(e);
                ST_ERROR("engine: %s (iteration %d, blocks %d)", fault.c_str(),
                         i, sb.blocks);
                break;
            }
            if (cancelled) {
                break;
            }
            const uint64_t here =
                static_cast<uint64_t>(r16(UC_X86_REG_CS)) * 16 +
                r16(UC_X86_REG_IP);
            if (here == SENTINEL) {
                clean = true;  // the ROM returned
                break;
            }
            if (!sb.irq_pending) {
                ST_WARN("engine: stalled at %04X:%04X after %d blocks",
                        r16(UC_X86_REG_CS), r16(UC_X86_REG_IP), sb.blocks);
                break;
            }
            sb.irq_pending = false;
            if (!commit()) {
                cancelled = true;
                break;
            }
            if (sb.finished || sb.blocks >= MAX_BLOCKS) {
                break;
            }
            if (!vector_irq(&pc)) {
                break;
            }
        }

        // Anything but a clean return leaves the ROM part-way through a far
        // call.  Re-entering with AL=7 happens to survive that, but AL=2
        // faults, so the image is rebuilt before the next call instead.
        dirty = !clean;
    }
};

// ---------------------------------------------------------------------------

engine::~engine() = default;

std::unique_ptr<engine> engine::create(std::wstring* error)
{
    const std::wstring dir = module_dir();
    const std::wstring candidates[] = {
        dir + L"engine.bin",
        dir + L"..\\engine.bin",
        dir + L"_smoothtalker_engine\\engine.bin",
        dir + L"..\\bin\\_smoothtalker_engine\\engine.bin",
    };
    for (const std::wstring& path : candidates) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return create_from(path, error);
        }
        ST_DEBUG("engine: not present: %ls", path.c_str());
    }
    if (error) {
        *error = L"engine.bin was not found next to " + dir;
    }
    ST_ERROR("engine: engine.bin was not found next to %ls", dir.c_str());
    return nullptr;
}

std::unique_ptr<engine> engine::create_from(const std::wstring& image_path,
                                            std::wstring* error)
{
    const unicorn_api* u = unicorn_load();
    if (!u) {
        if (error) {
            *error = unicorn_last_error();
        }
        return nullptr;
    }

    HANDLE h = CreateFileW(image_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (error) {
            *error = L"could not open " + image_path;
        }
        ST_ERROR("engine: could not open %ls: %s", image_path.c_str(),
                 win32_error_text(err).c_str());
        return nullptr;
    }
    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    std::vector<uint8_t> raw(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const BOOL ok =
        !raw.empty() && ReadFile(h, raw.data(), static_cast<DWORD>(raw.size()),
                                 &got, nullptr) && got == raw.size();
    CloseHandle(h);
    if (!ok) {
        if (error) {
            *error = L"could not read " + image_path;
        }
        ST_ERROR("engine: could not read %ls", image_path.c_str());
        return nullptr;
    }

    if (raw.size() < DUMP_HDR || std::memcmp(raw.data(), "SBR1", 4) != 0) {
        if (error) {
            *error = image_path + L" is not a SmoothTalker engine image";
        }
        ST_ERROR("engine: %ls has a bad magic header", image_path.c_str());
        return nullptr;
    }

    uint16_t hdr[6] = {0};
    std::memcpy(hdr, raw.data() + 4, sizeof(hdr));
    if (!(hdr[5] & 1)) {
        if (error) {
            *error = image_path + L" was captured without SBTALKER resident";
        }
        ST_ERROR("engine: %ls was captured without SBTALKER resident",
                 image_path.c_str());
        return nullptr;
    }

    std::unique_ptr<engine> eng(new engine());
    eng->p_ = std::make_unique<engine::impl>();
    eng->image_path_ = image_path;
    engine::impl& p = *eng->p_;
    p.u = u;
    p.image.assign(raw.begin() + DUMP_HDR, raw.end());
    p.res_seg = hdr[0];
    p.res_bx = hdr[1];
    p.ent_off = hdr[2];
    p.ent_seg = hdr[3];
    p.buf_off = static_cast<uint16_t>(p.res_bx + 0x20);

    if (p.image.size() > MEM_SIZE) {
        if (error) {
            *error = image_path + L" is larger than the memory map";
        }
        ST_ERROR("engine: image is %zu bytes, larger than the %llu byte map",
                 p.image.size(), static_cast<unsigned long long>(MEM_SIZE));
        return nullptr;
    }

    ST_INFO("engine: loaded %ls (%zu bytes) res_seg=%04X res_bx=%04X "
            "entry=%04X:%04X buf_off=%04X",
            image_path.c_str(), p.image.size(), p.res_seg, p.res_bx, p.ent_seg,
            p.ent_off, p.buf_off);
    return eng;
}

bool engine::configure(const engine_params& in)
{
    engine_params p = in;
    // Clamp here as well as in the settings layer, so that what the log says
    // we sent is exactly what the ROM received.
    p.gender = (std::max)(0, (std::min)(9, p.gender));
    p.tone = (std::max)(0, (std::min)(1, p.tone));
    p.volume = (std::max)(0, (std::min)(9, p.volume));
    p.pitch = (std::max)(0, (std::min)(9, p.pitch));
    p.speed = (std::max)(0, (std::min)(9, p.speed));

    impl& s = *p_;
    s.params = p;
    s.have_params = true;
    if (!s.ensure()) {
        return false;
    }
    // ensure() re-applies the parameters itself after a rebuild; only do it
    // here when it handed back an already-live emulator.
    if (!s.just_rebuilt) {
        s.apply(p);
    }
    ST_DEBUG("engine: configured tone=%d volume=%d pitch=%d speed=%d", p.tone,
             p.volume, p.pitch, p.speed);
    return !s.faulted;
}

bool engine::speak(const std::string& text_in, const block_fn& on_block,
                   const cancel_fn& should_cancel)
{
    std::string text = text_in;
    if (text.size() > static_cast<size_t>(MAX_TEXT)) {
        text.resize(MAX_TEXT);
    }
    if (text.empty()) {
        return true;
    }

    impl& s = *p_;
    if (!s.ensure()) {
        return false;
    }
    s.begin_run(&on_block, &should_cancel);

    // buffer[0] is a length byte and the text starts at buffer+1.  Clamp the
    // whole payload to the 0x100-byte text area so a full-length utterance
    // cannot spill into `outstring` at buffer+0x100.
    std::vector<uint8_t> payload;
    payload.reserve(0x100);
    payload.push_back(static_cast<uint8_t>(text.size()));
    payload.insert(payload.end(), text.begin(), text.end());
    payload.push_back('\r');
    payload.push_back('\0');
    payload.resize((std::min)(static_cast<size_t>(0x100), payload.size() + 8),
                   0);

    s.u->mem_write(s.uc, static_cast<uint64_t>(s.res_seg) * 16 + s.buf_off,
                   payload.data(), payload.size());
    s.setup_regs(FN_SPEAK);
    s.run_loop();

    native_rate_ = s.native_rate;

    if (s.faulted) {
        // Do not let a damaged emulator state poison later utterances.
        s.drop();
        return false;
    }
    return true;
}

bool engine::speak_all(const std::string& cp437_text, int out_rate,
                       std::vector<int16_t>& out, const cancel_fn& should_cancel)
{
    const std::vector<std::string> pieces = split_for_engine(cp437_text);
    if (pieces.empty()) {
        return true;
    }

    std::unique_ptr<resampler> rs;
    std::vector<int16_t> pcm16;
    std::vector<int16_t> resampled;

    block_fn sink = [&](const uint8_t* data, size_t count, int rate) -> bool {
        to_pcm16(data, count, pcm16);
        if (out_rate <= 0 || rate == out_rate) {
            out.insert(out.end(), pcm16.begin(), pcm16.end());
            return true;
        }
        // One resampler for the whole utterance, so the phase carries across
        // block and chunk boundaries and no click is left at the seams.
        if (!rs) {
            rs = std::make_unique<resampler>(rate, out_rate);
        }
        rs->feed(pcm16.data(), pcm16.size(), resampled);
        out.insert(out.end(), resampled.begin(), resampled.end());
        return true;
    };

    for (const std::string& piece : pieces) {
        if (should_cancel && should_cancel()) {
            return true;
        }
        if (!speak(piece, sink, should_cancel)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------

std::vector<std::string> split_for_engine(const std::string& text_in, int limit)
{
    // Collapse runs of whitespace first: the ROM has no use for them and
    // they only eat into the 255-character budget.
    std::string text;
    text.reserve(text_in.size());
    bool space = false;
    for (const char ch : text_in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
            c == '\v') {
            space = !text.empty();
        } else {
            if (space) {
                text.push_back(' ');
                space = false;
            }
            text.push_back(ch);
        }
    }

    std::vector<std::string> out;
    if (text.empty() || limit <= 0) {
        return out;
    }

    const auto trim = [](std::string s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) {
            return std::string();
        }
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    };

    while (text.size() > static_cast<size_t>(limit)) {
        const std::string window = text.substr(0, limit + 1);
        size_t cut = std::string::npos;
        for (const char* seps : {".!?", ",;:"}) {
            size_t best = std::string::npos;
            for (const char* sp = seps; *sp; ++sp) {
                const std::string pat = std::string(1, *sp) + " ";
                const size_t idx = window.rfind(pat);
                if (idx != std::string::npos &&
                    (best == std::string::npos || idx > best)) {
                    best = idx;
                }
            }
            // Ignore uselessly early breaks: cutting after the first few
            // characters would leave a scrap of an utterance behind.
            if (best != std::string::npos &&
                best > static_cast<size_t>(limit) / 4) {
                cut = best + 1;
                break;
            }
        }
        if (cut == std::string::npos) {
            cut = window.rfind(' ');
        }
        if (cut == std::string::npos || cut == 0) {
            cut = static_cast<size_t>(limit);  // one enormous word
        }
        std::string piece = trim(text.substr(0, cut));
        if (!piece.empty()) {
            out.push_back(piece);
        }
        text = trim(text.substr(cut));
    }
    if (!text.empty()) {
        out.push_back(text);
    }
    return out;
}

void to_pcm16(const uint8_t* pcm8, size_t count, std::vector<int16_t>& out)
{
    out.resize(count);
    for (size_t i = 0; i < count; ++i) {
        out[i] = static_cast<int16_t>((static_cast<int>(pcm8[i]) - 128) << 8);
    }
}

resampler::resampler(int src_rate, int dst_rate)
{
    if (src_rate <= 0 || dst_rate <= 0 || src_rate == dst_rate) {
        passthrough_ = true;
        ratio_ = 1.0;
    } else {
        ratio_ = static_cast<double>(src_rate) / static_cast<double>(dst_rate);
    }
}

void resampler::feed(const int16_t* src, size_t count, std::vector<int16_t>& out)
{
    out.clear();
    if (!count) {
        return;
    }
    if (passthrough_) {
        out.assign(src, src + count);
        return;
    }
    const double n = static_cast<double>(count);
    out.reserve(static_cast<size_t>(n / ratio_) + 2);
    double pos = pos_;
    while (pos < n) {
        const size_t i = static_cast<size_t>(pos);
        const double frac = pos - static_cast<double>(i);
        const int a = (i == 0) ? prev_ : src[i - 1];
        const int b = src[i];
        out.push_back(static_cast<int16_t>(
            static_cast<int>(a + (b - a) * frac)));
        pos += ratio_;
    }
    pos_ = pos - n;
    prev_ = src[count - 1];
}

std::string to_cp437(const wchar_t* text, size_t len)
{
    if (!text || !len) {
        return {};
    }
    // Fold the Unicode punctuation that word processors and web pages produce
    // onto the ASCII the ROM knows.  Without this a curly apostrophe becomes
    // the CP437 default character and the ROM says the word "question mark"
    // in the middle of a contraction.
    std::wstring s(text, len);
    for (wchar_t& c : s) {
        switch (c) {
        case 0x2018:
        case 0x2019:
        case 0x201B:
        case 0x2032: c = L'\''; break;
        case 0x201C:
        case 0x201D:
        case 0x201F:
        case 0x2033: c = L'"'; break;
        case 0x2010:
        case 0x2011:
        case 0x2012:
        case 0x2013:
        case 0x2014:
        case 0x2015:
        case 0x2212: c = L'-'; break;
        case 0x00A0:
        case 0x2007:
        case 0x2009:
        case 0x202F: c = L' '; break;
        case 0x2022:
        case 0x00B7: c = L'.'; break;
        default: break;
        }
    }
    // Ellipsis is the one that is not a 1:1 swap.
    for (size_t i = s.find(0x2026); i != std::wstring::npos;
         i = s.find(0x2026, i + 3)) {
        s.replace(i, 1, L"...");
    }

    const char fallback = ' ';
    BOOL used = FALSE;
    const int n = WideCharToMultiByte(437, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0,
                                      &fallback, &used);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(437, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n, &fallback, &used);
    return out;
}

}
