/*
 * Copyright (c) 1997, 2012, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2015, Red Hat Inc. All rights reserved.
 * Copyright (c) 2015, Linaro Ltd. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/java.hpp"
#include "runtime/stubCodeGenerator.hpp"
#include "vm_version_aarch32.hpp"
#ifdef TARGET_OS_FAMILY_linux
# include "os_linux.inline.hpp"
#endif

#ifndef AT_HWCAP
#define AT_HWCAP        16              /* Machine-dependent hints about
                                           processor capabilities.  */
#endif

#ifndef AT_HWCAP2
#define AT_HWCAP2       26              /* More machine-dependent hints about
                                           processor capabilities.  */
#endif

#ifndef HWCAP2_PMULL
#define HWCAP2_PMULL    (1 << 1)
#endif

#ifndef HWCAP2_AES
#define HWCAP2_AES      (1 << 0)
#endif

#ifndef HWCAP2_SHA1
#define HWCAP2_SHA1     (1 << 2)
#endif

#ifndef HWCAP2_SHA2
#define HWCAP2_SHA2     (1 << 3)
#endif

#ifndef HWCAP2_CRC32
#define HWCAP2_CRC32    (1 << 4)
#endif

#ifndef HWCAP_NEON
#define HWCAP_NEON      (1 << 12)
#endif

#ifndef HWCAP_VFPv3
#define HWCAP_VFPv3     (1 << 13)
#endif

#ifndef HWCAP_VFPv3D16
#define HWCAP_VFPv3D16  (1 << 14)       /* also set for VFPv4-D16 */
#endif

#ifndef HWCAP_TLS
#define HWCAP_TLS       (1 << 15)
#endif

#ifndef HWCAP_VFPv4
#define HWCAP_VFPv4     (1 << 16)
#endif

#ifndef HWCAP_IDIVA
#define HWCAP_IDIVA     (1 << 17)
#endif

#ifndef HWCAP_VFPD32
#define HWCAP_VFPD32    (1 << 19)       /* set if VFP has 32 regs (not 16) */
#endif

enum ProcessorFeatures VM_Version::_features = FT_NONE;
const char* VM_Version::_cpu_features = "";

static BufferBlob* stub_blob;
static const int stub_size = 550;

extern "C" {
  typedef void (*getPsrInfo_stub_t)(void*);
}
static getPsrInfo_stub_t getPsrInfo_stub = NULL;

typedef unsigned long (*pgetauxval)(unsigned long type);

class VM_Version_StubGenerator: public StubCodeGenerator {
 public:

  VM_Version_StubGenerator(CodeBuffer *c) : StubCodeGenerator(c) {}

  address generate_getPsrInfo() {
    StubCodeMark mark(this, "VM_Version", "getPsrInfo_stub");
#   define __ _masm->
    address start = __ pc();

    // void getPsrInfo(VM_Version::CpuidInfo* cpuid_info);

    address entry = __ pc();

    // TODO : redefine fields in CpuidInfo and generate
    // code to fill them in

    __ b(lr);

#   undef __

    return start;
  }
};


bool VM_Version::identify_procline(const char *tag, char **line) {
  char *i = *line;
  const char EOT = '\t', EOT2 = ':'; // the longest has no tabs
  for(; '\0' != *i && EOT != *i && EOT2 != *i; i++);
  if(EOT == *i || EOT2 == *i) {
    if(!memcmp(*line, tag, i - *line)) {
      for(i++; (EOT == *i || EOT2 == *i || ' ' == *i) && '\0' != *i; i++);
      if('\0' != *i) {
        *line = i;
        return true;
      }
    }
  }
  return false;
}

void VM_Version::get_processor_features() {
  _supports_cx8 = true;
  _supports_atomic_getset4 = true;
  _supports_atomic_getadd4 = true;
  _supports_atomic_getset8 = true;
  _supports_atomic_getadd8 = true;

  if (FLAG_IS_DEFAULT(AllocatePrefetchDistance))
    FLAG_SET_DEFAULT(AllocatePrefetchDistance, 256);
  if (FLAG_IS_DEFAULT(AllocatePrefetchStepSize))
    FLAG_SET_DEFAULT(AllocatePrefetchStepSize, 64);
  FLAG_SET_DEFAULT(PrefetchScanIntervalInBytes, 256);
  FLAG_SET_DEFAULT(PrefetchFieldsAhead, 256);
  FLAG_SET_DEFAULT(PrefetchCopyIntervalInBytes, 256);

  enum ProcessorFeatures f = FT_NONE;

  // try the recommended way, by using glibc API.
  // however since this API is only available in recent
  // versions of glibc we got to invoke it indirectly for
  // not to create compile and run-time dependency
  pgetauxval getauxval_ptr = (pgetauxval) os::dll_lookup((void*) 0, "getauxval");
  if (getauxval_ptr) {
    unsigned long auxv2 = (*getauxval_ptr)(AT_HWCAP2);
    unsigned long auxv = (*getauxval_ptr)(AT_HWCAP);
    if (FLAG_IS_DEFAULT(UseCRC32)) {
      UseCRC32 = (auxv2 & HWCAP2_CRC32) != 0;
    }
    if (auxv2 & HWCAP2_AES) {
      UseAES = UseAES || FLAG_IS_DEFAULT(UseAES);
      UseAESIntrinsics =
              UseAESIntrinsics || (UseAES && FLAG_IS_DEFAULT(UseAESIntrinsics));
      if (UseAESIntrinsics && !UseAES) {
        warning("UseAESIntrinsics enabled, but UseAES not, enabling");
        UseAES = true;
      }
    } else {
      if (UseAES) {
        warning("UseAES specified, but not supported on this CPU");
      }
      if (UseAESIntrinsics) {
        warning("UseAESIntrinsics specified, but not supported on this CPU");
      }
    }
    if (auxv & HWCAP_NEON)
      f = (ProcessorFeatures) (f | FT_AdvSIMD);
    if (auxv & HWCAP_IDIVA)
      f = (ProcessorFeatures) (f | FT_HW_DIVIDE);
    if (auxv & HWCAP_VFPv3)
      f = (ProcessorFeatures) (f | FT_VFPV3 | FT_VFPV2);
    if (auxv2 & HWCAP2_CRC32)
      f = (ProcessorFeatures) (f | FT_CRC32);
  }

  int ncores = 0, cpu, variant, model, revision;
  char buf[2048], *i;
  if (FILE * fp = fopen("/proc/cpuinfo", "r")) {
    while ((i = fgets(buf, 2048, fp))) {
      if (identify_procline("Features", &i)) {
        i = strtok(i, " \n");
        while (i) {
          if (!strcmp("idiva", i)) {
            f = (ProcessorFeatures) (f | FT_HW_DIVIDE);
          } else if (!strcmp("vfpv3", i) || !strcmp("vfpv4", i)) {
            // Assuming that vfpv4 implements all of vfpv3
            // and that they both implement all of v2.
            f = (ProcessorFeatures) (f | FT_VFPV3 | FT_VFPV2);
          } else if (!strcmp("vfp", i)) {
            // Assuming that VFPv2 is identified by plain vfp
            f = (ProcessorFeatures) (f | FT_VFPV2);
          } else if (!strcmp("neon", i)) {
            f = (ProcessorFeatures) (f | FT_AdvSIMD);
          }
          i = strtok(NULL, " \n");
        }
      } else if (identify_procline("Processor", &i)) {
        i = strtok(i, " \n");
        while (i) {
          // if the info is read correctly do
          if (!strcmp("ARMv7", i)) {
            f = (ProcessorFeatures) (f | FT_ARMV7);
          } else if (!strcmp("ARMv6-compatible", i)) {
            //TODO sort out the ARMv6 identification code
          }
          i = strtok(NULL, " \n");
        }
      } else if (identify_procline("model name", &i)) {
        i = strtok(i, " \n");
        while (i) {
          // if the info is read correctly do
          if (!strcmp("ARMv7", i) || !strcmp("AArch64", i)) {
            f = (ProcessorFeatures) (f | FT_ARMV7);
          } else if (!strcmp("ARMv6-compatible", i)) {
            //TODO sort out the ARMv6 identification code
          }
          i = strtok(NULL, " \n");
        }
      } else if (identify_procline("processor", &i)) {
        ncores++;
      } else if (identify_procline("CPU implementer", &i)) {
        cpu = strtol(i, NULL, 0);
      } else if (identify_procline("CPU variant", &i)) {
        variant = strtol(i, NULL, 0);
      } else if (identify_procline("CPU part", &i)) {
        model = strtol(i, NULL, 0);
      } else if (identify_procline("CPU revision", &i)) {
        revision = strtol(i, NULL, 0);
      }
    }
    fclose(fp);
  }
  if (1 == ncores) {
    f = (ProcessorFeatures) (f | FT_SINGLE_CORE);
  }
  if (FLAG_IS_DEFAULT(UseCRC32Intrinsics)) {
    UseCRC32Intrinsics = true;
  }
  if ((f & FT_AdvSIMD) && FLAG_IS_DEFAULT(UseNeon) && (model & ~0x0f0) >= 0xc08) {
    UseNeon = true;
  }
  _features = f;
  sprintf(buf, "0x%02x:0x%x:0x%03x:%d", cpu, variant, model, revision);
  _cpu_features = os::strdup(buf);

#ifdef COMPILER2
  if (UseMultiplyToLenIntrinsic) {
    if (!FLAG_IS_DEFAULT(UseMultiplyToLenIntrinsic)) {
      warning("multiplyToLen intrinsic is not available in 32-bit VM");
    }
    FLAG_SET_DEFAULT(UseMultiplyToLenIntrinsic, false);
  }
#endif // COMPILER2

/*  if (FLAG_IS_DEFAULT(UseBarriersForVolatile)) {
    UseBarriersForVolatile = (_cpuFeatures & CPU_DMB_ATOMICS) != 0;
  }*/

  /*if(!(f & FT_ARMV7) && FLAG_IS_DEFAULT(UseMembar)) {
  UseMembar = false;
  } else if(UseMembar) {
  fprintf(stderr, "Unable to use memory barriers as not on ARMv7, disabling.\n");
  UseMembar = false;
  }*/

  if (UseAES) {
    warning("AES instructions are not implemented on this CPU");
    FLAG_SET_DEFAULT(UseAES, false);
  }
  if (UseAESIntrinsics) {
    warning("AES intrinsics are not implemented on this CPU");
    FLAG_SET_DEFAULT(UseAESIntrinsics, false);
  }

  if (UseSHA) {
    warning("SHA instructions are not available on this CPU");
    FLAG_SET_DEFAULT(UseSHA, false);
  }
  if (UseSHA1Intrinsics || UseSHA256Intrinsics || UseSHA512Intrinsics) {
    warning("SHA intrinsics are not available on this CPU");
    FLAG_SET_DEFAULT(UseSHA1Intrinsics, false);
    FLAG_SET_DEFAULT(UseSHA256Intrinsics, false);
    FLAG_SET_DEFAULT(UseSHA512Intrinsics, false);
  }

}

void VM_Version::initialize() {
  ResourceMark rm;

  stub_blob = BufferBlob::create("getPsrInfo_stub", stub_size);
  if (stub_blob == NULL) {
    vm_exit_during_initialization("Unable to allocate getPsrInfo_stub");
  }

  CodeBuffer c(stub_blob);
  VM_Version_StubGenerator g(&c);
  getPsrInfo_stub = CAST_TO_FN_PTR(getPsrInfo_stub_t,
                                   g.generate_getPsrInfo());

  get_processor_features();

  //FIXME: turning off CriticalJNINatives flag while it is not implemented
  FLAG_SET_DEFAULT(CriticalJNINatives, false);
}
