#pragma once

#include <inery/chain/wasm_interface.hpp>
#include <inery/chain/webassembly/wavm.hpp>
#include <inery/chain/webassembly/wabt.hpp>
#ifdef INERY_INE_VM_OC_RUNTIME_ENABLED
#include <inery/chain/webassembly/ine-vm-oc.hpp>
#else
#define _REGISTER_INEVMOC_INTRINSIC(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)
#endif
#include <inery/chain/webassembly/runtime_interface.hpp>
#include <inery/chain/wasm_inery_injection.hpp>
#include <inery/chain/transaction_context.hpp>
#include <inery/chain/code_object.hpp>
#include <inery/chain/exceptions.hpp>
#include <fc/scoped_exit.hpp>

#include "IR/Module.h"
#include "Runtime/Intrinsics.h"
#include "Platform/Platform.h"
#include "WAST/WAST.h"
#include "IR/Validate.h"

#if defined(INERY_INE_VM_RUNTIME_ENABLED) || defined(INERY_INE_VM_JIT_RUNTIME_ENABLED)
#include <inery/chain/webassembly/ine-vm.hpp>
#include <inery/vm/allocator.hpp>
#else
#define _REGISTER_INE_VM_INTRINSIC(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)
#endif

using namespace fc;
using namespace inery::chain::webassembly;
using namespace IR;
using namespace Runtime;

using boost::multi_index_container;

namespace inery { namespace chain {

   namespace inevmoc { struct config; }

   struct wasm_interface_impl {
      struct wasm_cache_entry {
         digest_type                                          code_hash;
         uint32_t                                             first_block_num_used;
         uint32_t                                             last_block_num_used;
         std::unique_ptr<wasm_instantiated_module_interface>  module;
         uint8_t                                              vm_type = 0;
         uint8_t                                              vm_version = 0;
      };
      struct by_hash;
      struct by_first_block_num;
      struct by_last_block_num;

#ifdef INERY_INE_VM_OC_RUNTIME_ENABLED
      struct inevmoc_tier {
         inevmoc_tier(const boost::filesystem::path& d, const inevmoc::config& c, const chainbase::database& db) : cc(d, c, db), exec(cc) {}
         inevmoc::code_cache_async cc;
         inevmoc::executor exec;
         inevmoc::memory mem;
      };
#endif

      wasm_interface_impl(wasm_interface::vm_type vm, bool inevmoc_tierup, const chainbase::database& d, const boost::filesystem::path data_dir, const inevmoc::config& inevmoc_config) : db(d), wasm_runtime_time(vm) {
         if(vm == wasm_interface::vm_type::wabt)
            runtime_interface = std::make_unique<webassembly::wabt_runtime::wabt_runtime>();
#ifdef INERY_INE_VM_RUNTIME_ENABLED
         if(vm == wasm_interface::vm_type::ine_vm)
            runtime_interface = std::make_unique<webassembly::ine_vm_runtime::ine_vm_runtime<inery::vm::interpreter>>();
#endif
#ifdef INERY_INE_VM_JIT_RUNTIME_ENABLED
         if(vm == wasm_interface::vm_type::ine_vm_jit)
            runtime_interface = std::make_unique<webassembly::ine_vm_runtime::ine_vm_runtime<inery::vm::jit>>();
#endif
#ifdef INERY_INE_VM_OC_RUNTIME_ENABLED
         if(vm == wasm_interface::vm_type::ine_vm_oc)
            runtime_interface = std::make_unique<webassembly::inevmoc::inevmoc_runtime>(data_dir, inevmoc_config, d);
#endif
         if(!runtime_interface)
            INE_THROW(wasm_exception, "${r} wasm runtime not supported on this platform and/or configuration", ("r", vm));

#ifdef INERY_INE_VM_OC_RUNTIME_ENABLED
         if(inevmoc_tierup) {
            INE_ASSERT(vm != wasm_interface::vm_type::ine_vm_oc, wasm_exception, "You can't use INE VM OC as the base runtime when tier up is activated");
            inevmoc.emplace(data_dir, inevmoc_config, d);
         }
#endif
      }

      ~wasm_interface_impl() {
         if(is_shutting_down)
            for(wasm_cache_index::iterator it = wasm_instantiation_cache.begin(); it != wasm_instantiation_cache.end(); ++it)
               wasm_instantiation_cache.modify(it, [](wasm_cache_entry& e) {
                  e.module.release();
               });
      }

      std::vector<uint8_t> parse_initial_memory(const Module& module) {
         std::vector<uint8_t> mem_image;

         for(const DataSegment& data_segment : module.dataSegments) {
            INE_ASSERT(data_segment.baseOffset.type == InitializerExpression::Type::i32_const, wasm_exception, "");
            INE_ASSERT(module.memories.defs.size(), wasm_exception, "");
            const U32 base_offset = data_segment.baseOffset.i32;
            const Uptr memory_size = (module.memories.defs[0].type.size.min << IR::numBytesPerPageLog2);
            if(base_offset >= memory_size || base_offset + data_segment.data.size() > memory_size)
               FC_THROW_EXCEPTION(wasm_execution_error, "WASM data segment outside of valid memory range");
            if(base_offset + data_segment.data.size() > mem_image.size())
               mem_image.resize(base_offset + data_segment.data.size(), 0x00);
            memcpy(mem_image.data() + base_offset, data_segment.data.data(), data_segment.data.size());
         }

         return mem_image;
      }

      void code_block_num_last_used(const digest_type& code_hash, const uint8_t& vm_type, const uint8_t& vm_version, const uint32_t& block_num) {
         wasm_cache_index::iterator it = wasm_instantiation_cache.find(boost::make_tuple(code_hash, vm_type, vm_version));
         if(it != wasm_instantiation_cache.end())
            wasm_instantiation_cache.modify(it, [block_num](wasm_cache_entry& e) {
               e.last_block_num_used = block_num;
            });
      }

      void current_lib(uint32_t lib) {
         //anything last used before or on the LIB can be evicted
         const auto first_it = wasm_instantiation_cache.get<by_last_block_num>().begin();
         const auto last_it  = wasm_instantiation_cache.get<by_last_block_num>().upper_bound(lib);
#ifdef INERY_INE_VM_OC_RUNTIME_ENABLED
         if(inevmoc) for(auto it = first_it; it != last_it; it++)
            inevmoc->cc.free_code(it->code_hash, it->vm_version);
#endif
         wasm_instantiation_cache.get<by_last_block_num>().erase(first_it, last_it);
      }

      const std::unique_ptr<wasm_instantiated_module_interface>& get_instantiated_module( const digest_type& code_hash, const uint8_t& vm_type,
                                                                                 const uint8_t& vm_version, transaction_context& trx_context )
      {
         wasm_cache_index::iterator it = wasm_instantiation_cache.find(
                                             boost::make_tuple(code_hash, vm_type, vm_version) );
         const code_object* codeobject = nullptr;
         if(it == wasm_instantiation_cache.end()) {
            codeobject = &db.get<code_object,by_code_hash>(boost::make_tuple(code_hash, vm_type, vm_version));

            it = wasm_instantiation_cache.emplace( wasm_interface_impl::wasm_cache_entry{
                                                      .code_hash = code_hash,
                                                      .first_block_num_used = codeobject->first_block_used,
                                                      .last_block_num_used = UINT32_MAX,
                                                      .module = nullptr,
                                                      .vm_type = vm_type,
                                                      .vm_version = vm_version
                                                   } ).first;
         }

         if(!it->module) {
            if(!codeobject)
               codeobject = &db.get<code_object,by_code_hash>(boost::make_tuple(code_hash, vm_type, vm_version));

            auto timer_pause = fc::make_scoped_exit([&](){
               trx_context.resume_billing_timer();
            });
            trx_context.pause_billing_timer();
            IR::Module module;
            std::vector<U8> bytes = {
                (const U8*)codeobject->code.data(),
                (const U8*)codeobject->code.data() + codeobject->code.size()};
            try {
               Serialization::MemoryInputStream stream((const U8*)bytes.data(),
                                                       bytes.size());
               WASM::serialize(stream, module);
               module.userSections.clear();
            } catch (const Serialization::FatalSerializationException& e) {
               INE_ASSERT(false, wasm_serialization_error, e.message.c_str());
            } catch (const IR::ValidationException& e) {
               INE_ASSERT(false, wasm_serialization_error, e.message.c_str());
            }
            if (runtime_interface->inject_module(module)) {
               try {
                  Serialization::ArrayOutputStream outstream;
                  WASM::serialize(outstream, module);
                  bytes = outstream.getBytes();
               } catch (const Serialization::FatalSerializationException& e) {
                  INE_ASSERT(false, wasm_serialization_error,
                             e.message.c_str());
               } catch (const IR::ValidationException& e) {
                  INE_ASSERT(false, wasm_serialization_error,
                             e.message.c_str());
               }
            }

            wasm_instantiation_cache.modify(it, [&](auto& c) {
               c.module = runtime_interface->instantiate_module((const char*)bytes.data(), bytes.size(), parse_initial_memory(module), code_hash, vm_type, vm_version);
            });
         }
         return it->module;
      }

      bool is_shutting_down = false;
      std::unique_ptr<wasm_runtime_interface> runtime_interface;

      typedef boost::multi_index_container<
         wasm_cache_entry,
         indexed_by<
            ordered_unique<tag<by_hash>,
               composite_key< wasm_cache_entry,
                  member<wasm_cache_entry, digest_type, &wasm_cache_entry::code_hash>,
                  member<wasm_cache_entry, uint8_t,     &wasm_cache_entry::vm_type>,
                  member<wasm_cache_entry, uint8_t,     &wasm_cache_entry::vm_version>
               >
            >,
            ordered_non_unique<tag<by_first_block_num>, member<wasm_cache_entry, uint32_t, &wasm_cache_entry::first_block_num_used>>,
            ordered_non_unique<tag<by_last_block_num>, member<wasm_cache_entry, uint32_t, &wasm_cache_entry::last_block_num_used>>
         >
      > wasm_cache_index;
      wasm_cache_index wasm_instantiation_cache;

      const chainbase::database& db;
      const wasm_interface::vm_type wasm_runtime_time;

#ifdef INERY_INE_VM_OC_RUNTIME_ENABLED
      fc::optional<inevmoc_tier> inevmoc;
#endif
   };

#define _ADD_PAREN_1(...) ((__VA_ARGS__)) _ADD_PAREN_2
#define _ADD_PAREN_2(...) ((__VA_ARGS__)) _ADD_PAREN_1
#define _ADD_PAREN_1_END
#define _ADD_PAREN_2_END
#define _WRAPPED_SEQ(SEQ) BOOST_PP_CAT(_ADD_PAREN_1 SEQ, _END)

#define _REGISTER_INTRINSIC_EXPLICIT(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)\
   _REGISTER_WAVM_INTRINSIC(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)         \
   _REGISTER_WABT_INTRINSIC(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)         \
   _REGISTER_INE_VM_INTRINSIC(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)       \
   _REGISTER_INEVMOC_INTRINSIC(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)

#define _REGISTER_INTRINSIC4(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)\
   _REGISTER_INTRINSIC_EXPLICIT(CLS, MOD, METHOD, WASM_SIG, NAME, SIG )

#define _REGISTER_INTRINSIC3(CLS, MOD, METHOD, WASM_SIG, NAME)\
   _REGISTER_INTRINSIC_EXPLICIT(CLS, MOD, METHOD, WASM_SIG, NAME, decltype(&CLS::METHOD) )

#define _REGISTER_INTRINSIC2(CLS, MOD, METHOD, WASM_SIG)\
   _REGISTER_INTRINSIC_EXPLICIT(CLS, MOD, METHOD, WASM_SIG, BOOST_PP_STRINGIZE(METHOD), decltype(&CLS::METHOD) )

#define _REGISTER_INTRINSIC1(CLS, MOD, METHOD)\
   static_assert(false, "Cannot register " BOOST_PP_STRINGIZE(CLS) ":" BOOST_PP_STRINGIZE(METHOD) " without a signature");

#define _REGISTER_INTRINSIC0(CLS, MOD, METHOD)\
   static_assert(false, "Cannot register " BOOST_PP_STRINGIZE(CLS) ":<unknown> without a method name and signature");

#define _UNWRAP_SEQ(...) __VA_ARGS__

#define _EXPAND_ARGS(CLS, MOD, INFO)\
   ( CLS, MOD, _UNWRAP_SEQ INFO )

#define _REGISTER_INTRINSIC(R, CLS, INFO)\
   BOOST_PP_CAT(BOOST_PP_OVERLOAD(_REGISTER_INTRINSIC, _UNWRAP_SEQ INFO) _EXPAND_ARGS(CLS, "env", INFO), BOOST_PP_EMPTY())

#define REGISTER_INTRINSICS(CLS, MEMBERS)\
   BOOST_PP_SEQ_FOR_EACH(_REGISTER_INTRINSIC, CLS, _WRAPPED_SEQ(MEMBERS))

#define _REGISTER_INJECTED_INTRINSIC(R, CLS, INFO)\
   BOOST_PP_CAT(BOOST_PP_OVERLOAD(_REGISTER_INTRINSIC, _UNWRAP_SEQ INFO) _EXPAND_ARGS(CLS, INERY_INJECTED_MODULE_NAME, INFO), BOOST_PP_EMPTY())

#define REGISTER_INJECTED_INTRINSICS(CLS, MEMBERS)\
   BOOST_PP_SEQ_FOR_EACH(_REGISTER_INJECTED_INTRINSIC, CLS, _WRAPPED_SEQ(MEMBERS))

} } // inery::chain
