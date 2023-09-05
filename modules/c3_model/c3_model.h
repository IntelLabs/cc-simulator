/*
 Copyright 2016 Intel Corporation
 SPDX-License-Identifier: MIT
*/
#ifndef MODULES_C3_MODEL_C3_MODEL_H_
#define MODULES_C3_MODEL_C3_MODEL_H_

#include <cassert>
#include <memory>
#include <string>
#include <simics/simulator/control.h>
#include <simics/simulator/memory.h>
extern "C" {
#include <xed-interface.h>
}
#include "ccsimics/c3_base_model.h"
#include "ccsimics/data_encryption.h"
#include "ccsimics/integrity.h"
#include "ccsimics/rep_movsb_tripwire.h"
#include "ccsimics/simics_util.h"

namespace ccsimics {

template <typename ConTy, typename CtxTy, typename PtrEncTy>
class C3Model : public C3BaseModel<ConTy, CtxTy, PtrEncTy> {
    using BaseTy = C3BaseModel<ConTy, CtxTy, PtrEncTy>;
    using IntegrityTy = ccsimics::Integrity<BaseTy, ConTy, CtxTy>;
    using RepMovsIsaTy = ccsimics::RepMovsTripwire<C3Model, ConTy, CtxTy>;

    std::unique_ptr<RepMovsIsaTy> rep_movs_;
    std::unique_ptr<IntegrityTy> integrity_;

 public:
    C3Model(ConTy *con, CtxTy *ctx, PtrEncTy *ptrenc)
        : C3BaseModel<ConTy, CtxTy, PtrEncTy>(con, ctx, ptrenc) {
        rep_movs_ = std::make_unique<RepMovsIsaTy>(this, con, ctx);
        integrity_ = std::make_unique<IntegrityTy>(this, con, ctx);
    }

    inline auto *get_integrity() { return integrity_.get(); }

    /** Register callbacks, uses template parameter to perform static casts
     * in the callback functions (i.e., not needing RTTI to do dynamic casts).
     */
    template <typename HandlerTy> inline void register_callbacks(ConTy *con) {
        BaseTy::template register_callbacks<HandlerTy>(con);

        con->register_exception_before_cb(
                CPU_Exception_All,
                [](auto *obj, auto *cpu, auto *handle, auto *m) {
                    static_cast<HandlerTy *>(m)->exception_before(obj, cpu,
                                                                  handle, NULL);
                },
                static_cast<void *>(this));

        rep_movs_->register_callbacks();
    }

    inline uint64_t encrypt_decrypt_u64(const uint64_t tweak,
                                        const uint64_t val,
                                        const data_key_t *k) const {
        uint8 buff[8];
        uint8 input_buff[8];
        ptr_metadata_t md;

        {
            uint64_t tmp_val = val;
            for (int i = 0; i < 8; ++i) {
                input_buff[i] = tmp_val & 0xff;
                tmp_val = tmp_val >> 8;
            }
        }

        cpu_bytes_t input{.size = 8, .data = input_buff};

        input.data = input_buff;

        cpu_bytes_t bytes_mod =
                encrypt_decrypt_bytes(&md, tweak, k, input, buff);
        uint64_t result = 0;
        for (int i = 0; i < 8; ++i) {
            result = result | (((uint64_t)bytes_mod.data[i]) << (i * 8));
        }
        return result;
    }

    inline uint64_t encrypt_decrypt_u64(uint64_t tweak, uint64_t val) const {
        return encrypt_decrypt_u64(tweak, val, this->ctx_->get_dp_key());
    }

    inline void set_integrity(bool val) {
        // Set the configuration exposed to target via C3 ctx ISA
        this->ctx_->set_icv_enabled(val);
        if (val) {
            integrity_->enable();
        } else {
            integrity_->disable();
        }
    }

    inline void
    set_integrity_supress_mode(enum ccsimics::INTEGRITY_SUPPRESS_MODE mode) {
        integrity_->set_suppress_icv_mode(mode);
    }

    inline void clear_icv_map() { integrity_->clear_icv_map(); }

    /**
     * @brief Perform optional fixup of LA/CA prior to memory access
     *
     * Should not directly change state of internal LA/CA variables, and instead
     * just returns the fixed up CA/LA. Initially intended to allow artificial
     * bypassing of C3 for demo / debug purposes.
     *
     * @return Fixed LA/CA (default imlelmentation returns input ptr unchanged)
     */
    template <typename T> inline T perform_ptr_fixup(const T ptr) const {
        if (integrity_) {
            return integrity_->do_icv_correction(ptr);
        }
        return ptr;
    }

    inline bool
    should_print_data_modification(const enum RW rw) const override {
        if (integrity_ && integrity_->icv_mismatch_in_cycle(rw)) {
            return true;
        }

        return BaseTy::should_print_data_modification(rw);
    }

    inline void modify_data_on_mem_access(memory_handle_t *mem, enum RW rw) {
        if (integrity_) {
            integrity_->check_integrity(mem, rw);
        }
        return BaseTy::modify_data_on_mem_access(mem, rw);
    }

    inline bool
    handle_la_mismatch_on_mem_access(logical_address_t la) override {
        if (integrity_ && integrity_->is_icv_corrected()) {
            // This is looks to be due to ICV correction, so we can ignore the
            // problem and not check further. But let's also double-check that
            // this doesn't mess with the page crossing logic
            ASSERT(!this->is_crossing_page_second_);
            ASSERT(!this->is_crossing_page_first_);
            return true;
        }

        return BaseTy::handle_la_mismatch_on_mem_access(la);
    }

    static inline void register_attributes(conf_class_t *cl) {
        IntegrityTy::register_attributes(cl);
    }
};

}  // namespace ccsimics

#endif  // MODULES_C3_MODEL_C3_MODEL_H_
