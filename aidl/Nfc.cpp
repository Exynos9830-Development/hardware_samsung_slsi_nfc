/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Nfc.h"

#include <android-base/logging.h>
#include <android-base/properties.h>

#include "NfcExtra.h"
#include "HalSecNfc.h"
#include "hal.h"

// HIDL passed the CORE_INIT_RSP packet into this legacy Samsung HAL
// entry point. The AIDL shim can still use it for older blobs when
// ro/persist.vendor.nfc.legacy_core_init_rsp is enabled.
extern int nfc_hal_core_initialized(uint8_t* p_core_init_rsp_params);

namespace aidl {
namespace android {
namespace hardware {
namespace nfc {

std::shared_ptr<INfcClientCallback> Nfc::mCallback = nullptr;
std::mutex Nfc::mCoreInitRspLock;
std::vector<uint8_t> Nfc::mLastCoreInitRsp;
AIBinder_DeathRecipient* clientDeathRecipient = nullptr;
pthread_mutex_t mLockOpenClose = PTHREAD_MUTEX_INITIALIZER;

void OnDeath(void* cookie) {
  LOG(INFO) << __func__ << "cookie : " << (*(uint64_t*)cookie);
    if (Nfc::mCallback != nullptr &&
        !AIBinder_isAlive(Nfc::mCallback->asBinder().get())) {
      LOG(INFO) << __func__ << " Nfc service has died";
      Nfc* nfc = static_cast<Nfc*>(cookie);
      nfc->close(NfcCloseType::DISABLE);
    }
  }

::ndk::ScopedAStatus Nfc::open(
    const std::shared_ptr<INfcClientCallback>& clientCallback) {
  LOG(INFO) << "Nfc:open";
  if (clientCallback == nullptr) {
    LOG(INFO) << "Nfc::open null callback";
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(NfcStatus::FAILED));
  } else {
    Nfc::mCallback = clientCallback;

    {
      std::lock_guard<std::mutex> lock(mCoreInitRspLock);
      mLastCoreInitRsp.clear();
    }
    LOG(INFO) << "open: cleared cached CORE_INIT_RSP";

    clientDeathRecipient = AIBinder_DeathRecipient_new(OnDeath);
    auto linkRet =
        AIBinder_linkToDeath(clientCallback->asBinder().get(),
                             clientDeathRecipient, this /* cookie */);
    if (linkRet != STATUS_OK) {
      LOG(ERROR) << __func__ << ": linkToDeath failed: " << linkRet;
      // Just ignore the error.
    }

    nfc_hal_enableAidl(true);
    int ret = nfc_hal_open(eventCallback, dataCallback);
    LOG(INFO) << "Nfc::open Exit count ";
    return ret == 0 ? ndk::ScopedAStatus::ok()
                    : ndk::ScopedAStatus::fromServiceSpecificError(
                          static_cast<int32_t>(NfcStatus::FAILED));
  }
}

::ndk::ScopedAStatus Nfc::close(NfcCloseType type) {
  LOG(INFO) << "close";
  if (Nfc::mCallback == nullptr) {
    LOG(ERROR) << __func__ << "mCallback null";
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(NfcStatus::FAILED));
  }
  int ret = 0;
  if (type == NfcCloseType::HOST_SWITCHED_OFF) {
    ret = nfc_hal_closeForPowerOffCase();
  } else {
    ret = nfc_hal_close();
  }
  {
    std::lock_guard<std::mutex> lock(mCoreInitRspLock);
    mLastCoreInitRsp.clear();
  }
  LOG(INFO) << "close: cleared cached CORE_INIT_RSP";
  Nfc::mCallback = nullptr;
  AIBinder_DeathRecipient_delete(clientDeathRecipient);
  clientDeathRecipient = nullptr;

  if(ret == 0)
    LOG(INFO) << "Nfc::close: ret = 0";
  else
    LOG(INFO) << "Nfc::close: ret != 0";

  return ret == 0 ? ndk::ScopedAStatus::ok()
                  : ndk::ScopedAStatus::fromServiceSpecificError(
                        static_cast<int32_t>(NfcStatus::FAILED));
}

::ndk::ScopedAStatus Nfc::coreInitialized() {
  LOG(INFO) << "Nfc::coreInitialized";
  if (Nfc::mCallback == nullptr) {
    LOG(ERROR) << __func__ << "mCallback null";
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(NfcStatus::FAILED));
  }

  std::vector<uint8_t> coreInitRsp;
  {
    std::lock_guard<std::mutex> lock(mCoreInitRspLock);
    coreInitRsp = mLastCoreInitRsp;
  }

  const bool legacyCoreInitRsp =
      ::android::base::GetBoolProperty("persist.vendor.nfc.legacy_core_init_rsp",
                                       ::android::base::GetBoolProperty(
                                           "ro.vendor.nfc.legacy_core_init_rsp",
                                           false));
  LOG(INFO) << "coreInitialized: legacy_core_init_rsp=" << legacyCoreInitRsp
            << " cached_len=" << coreInitRsp.size();

  int ret;
  if (legacyCoreInitRsp && !coreInitRsp.empty()) {
    LOG(INFO) << "coreInitialized: forwarding cached CORE_INIT_RSP len="
              << coreInitRsp.size();
    ret = nfc_hal_core_initialized(coreInitRsp.data());
  } else {
    if (legacyCoreInitRsp) {
      LOG(WARNING) << "coreInitialized: legacy CORE_INIT_RSP enabled, "
                   << "but no cached CORE_INIT_RSP is available; falling back";
    }
    ret = nfc_hal_core_initialized_for_aidl();
  }
  if(ret == 0)
    LOG(INFO) << "coreInitialized: ret = 0, so call ok()";
  else
    LOG(ERROR) << "coreInitialized: ret != 0, so call Error()";

  return ret == 0 ? ndk::ScopedAStatus::ok()
                  : ndk::ScopedAStatus::fromServiceSpecificError(
                        static_cast<int32_t>(NfcStatus::FAILED));
}

::ndk::ScopedAStatus Nfc::factoryReset() {
  LOG(INFO) << "factoryReset";
  nfc_hal_factory_reset();
  return ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus Nfc::getConfig(NfcConfig* _aidl_return) {
  LOG(INFO) << "getConfig";
  NfcConfig nfcVendorConfig;
  nfc_hal_getVendorConfig_aidl(nfcVendorConfig);

  *_aidl_return = nfcVendorConfig;
  return ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus Nfc::powerCycle() {
  LOG(INFO) << "powerCycle";
  if (Nfc::mCallback == nullptr) {
    LOG(ERROR) << __func__ << "mCallback null";
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(NfcStatus::FAILED));
  }
  return nfc_hal_power_cycle() ? ndk::ScopedAStatus::fromServiceSpecificError(
                                       static_cast<int32_t>(NfcStatus::FAILED))
                                 : ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus Nfc::preDiscover() {
  if (Nfc::mCallback == nullptr) {
    LOG(ERROR) << __func__ << "mCallback null";
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(NfcStatus::FAILED));
  }
  return nfc_hal_pre_discover()
             ? ndk::ScopedAStatus::fromServiceSpecificError(
                   static_cast<int32_t>(NfcStatus::FAILED))
             : ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus Nfc::write(const std::vector<uint8_t>& data,
                                int32_t* _aidl_return) {
  if (Nfc::mCallback == nullptr) {
    LOG(ERROR) << __func__ << "mCallback null";
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(NfcStatus::FAILED));
  }
  *_aidl_return = nfc_hal_write(data.size(), &data[0]);
  return ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus Nfc::setEnableVerboseLogging(bool enable) {
  LOG(INFO) << "setEnableVerboseLogging : enable = " << enable;
  nfc_hal_setLogging(enable);
  return ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus Nfc::isVerboseLoggingEnabled(bool* _aidl_return) {
  LOG(INFO) << "isVerboseLoggingEnabled";
  *_aidl_return = nfc_hal_isLoggingEnabled();
  LOG(INFO) << "isVerboseLoggingEnabled : _aidl_return = " << (*_aidl_return);
  return ndk::ScopedAStatus::ok();
}

}  // namespace nfc
}  // namespace hardware
}  // namespace android
}  // namespace aidl
