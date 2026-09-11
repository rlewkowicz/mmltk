// Validation-only input capabilities for Ubuntu Weston 13.0.0-4build3.
#include <libweston/libweston.h>

#include <memory>
#include <new>

// These exported functions are private in Weston 13. The builder and runtime
// packages must retain the identical version pinned in the validation image.
// Declarations: wayland/weston tag 13.0.0, libweston/libweston-internal.h.
// Ownership: libweston/input.c; upstream fake-seat change:
// 0126a5b4fc03c3ac7f023d87303bf09f7d494ac1.
extern "C" {
void weston_seat_init(weston_seat*, weston_compositor*, const char*);
int weston_seat_init_pointer(weston_seat*);
int weston_seat_init_keyboard(weston_seat*, xkb_keymap*);
void weston_seat_release(weston_seat*);
}

namespace {
class HeadlessSeat final {
public:
    explicit HeadlessSeat(weston_compositor* compositor) noexcept {
        destroy_.owner = this;
        destroy_.notify = Destroy;
        wl_list_init(&destroy_.link);
        weston_seat_init(&seat_, compositor, "default");
    }

    ~HeadlessSeat() {
        wl_list_remove(&destroy_.link);
        weston_seat_release(&seat_);
    }

    HeadlessSeat(const HeadlessSeat&) = delete;
    HeadlessSeat& operator=(const HeadlessSeat&) = delete;

    bool Install(weston_compositor* compositor) noexcept {
        if (weston_seat_init_pointer(&seat_) < 0 ||
            weston_seat_init_keyboard(&seat_, nullptr) < 0) {
            return false;
        }
        wl_signal_add(&compositor->destroy_signal, &destroy_);
        return true;
    }

private:
    struct DestroyListener : wl_listener {
        HeadlessSeat* owner = nullptr;
    };

    static void Destroy(wl_listener* listener, void*) noexcept {
        delete static_cast<DestroyListener*>(listener)->owner;
    }

    weston_seat seat_{};
    DestroyListener destroy_{};
};
}

extern "C" WL_EXPORT int wet_module_init(
    weston_compositor* compositor, int*, char*[]) {
    auto seat = std::unique_ptr<HeadlessSeat>(new (std::nothrow) HeadlessSeat(compositor));
    if (!seat || !seat->Install(compositor)) {
        return -1;
    }
    // The compositor's destroy listener now owns the seat and both devices.
    seat.release();
    return 0;
}
