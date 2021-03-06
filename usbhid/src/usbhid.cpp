// usbhid.cpp : Defines the exported functions for the DLL application.
//
//  MIT License
//  See LICENSE.txt file in root of project
//  Copyright(c) 2018 Simon Parmenter


#include "stdafx.h"

#include "../interface/usbhid.hpp"

#include <hidsdi.h>
#include <SetupAPI.h>

#include <cassert>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>


namespace
{
  // See https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/content/hidsdi/nf-hidsdi-hidd_getmanufacturerstring
  std::size_t const MAX_USB_DEVICE_STR_LEN{126}; 

  ::GUID GetHidGuid() noexcept;

  ::GUID const g_hidGUID{ GetHidGuid() };

  std::wstring GetDevicePath(::HDEVINFO hDevInfo, ::SP_DEVICE_INTERFACE_DATA & diData) noexcept;
}


// ################### interface #############################################


USBHID_ns::installedDeviceInfoList_t USBHID_ns::GetlInstalledDevicesInfo() noexcept
{
  try
  {
    auto const & deviceInfo{ ::SetupDiGetClassDevs(&g_hidGUID, nullptr, nullptr, DIGCF_PRESENT | DIGCF_INTERFACEDEVICE) };
    if(deviceInfo != INVALID_HANDLE_VALUE)
    {
      std::vector<std::wstring> const devicePaths
      {
        [&deviceInfo]
        {
          DWORD memberIndex{};
          ::SP_DEVICE_INTERFACE_DATA deviceInterfaceData{ sizeof ::SP_DEVICE_INTERFACE_DATA };
          std::vector<std::wstring> devPaths;
          while(::SetupDiEnumDeviceInterfaces(deviceInfo, nullptr, &g_hidGUID, memberIndex++, &deviceInterfaceData))
          {
            auto const & devicePath{ GetDevicePath(deviceInfo, deviceInterfaceData) };
            if(!devicePath.empty())
            {
              devPaths.emplace_back(devicePath);
            }
          }
          return devPaths;
        }()
      };

      using deviceHandlePathMap_t = std::map<::HANDLE, std::wstring>;
      deviceHandlePathMap_t const openDeviceHandles
      {
        [&devicePaths]
        {
        deviceHandlePathMap_t handles;
          for(auto const & devPath : devicePaths)
          {
            auto const & handle{::CreateFile(devPath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
            if(handle != INVALID_HANDLE_VALUE)
            {
              // no check for already-existing element
              handles.emplace(handle, devPath);
            }
          }
          return handles;
        }()
      };

      class CloseHandlesGuard
      {
      public:
        CloseHandlesGuard(deviceHandlePathMap_t const & deviceHandleMap) : m_deviceHandleMap{ deviceHandleMap } {}
        ~CloseHandlesGuard()
        {
          std::for_each(std::cbegin(m_deviceHandleMap), std::cend(m_deviceHandleMap), [](auto const & elem) {::CloseHandle(elem.first); });
        }

      private:
        deviceHandlePathMap_t const & m_deviceHandleMap;
      }closeHandlesGuard(openDeviceHandles);

      using handleManufacturerMap_t = std::map<::HANDLE, std::wstring>;
      handleManufacturerMap_t const hndManufacturerMap
      {
        [&openDeviceHandles]
        {
          handleManufacturerMap_t names;
          for(auto const & elem : openDeviceHandles)
          {
            auto const & hnd{elem.first};
            std::wstring str(MAX_USB_DEVICE_STR_LEN, L'\0');
            ::HidD_GetManufacturerString(elem.first, str.data(), MAX_USB_DEVICE_STR_LEN);
            auto const endOfStr{ str.find_first_of(L'\0') };
            str.erase(endOfStr);
            names.emplace(hnd, std::move(str));
          }
          return names;
        }()
      };


      using handleAttributesMap_t = std::map<::HANDLE, hidAttributes>;
      handleAttributesMap_t const handleAttributesMap
      {
        [&openDeviceHandles]
        {
          ::HIDD_ATTRIBUTES attribs{sizeof ::HIDD_ATTRIBUTES };
          handleAttributesMap_t handleAttributes;
          for(auto const & elem : openDeviceHandles)
          {
            auto const & hnd{ elem.first };
            if(::HidD_GetAttributes(elem.first, &attribs))
            {
              hidAttributes attributes{ attribs.VendorID, attribs.ProductID, attribs.VersionNumber };
              handleAttributes.emplace(hnd, attributes);
            }
          }
          return handleAttributes;
        }()
      };

      using parsedDataPtr = std::unique_ptr<::_HIDP_PREPARSED_DATA, decltype(&::HidD_FreePreparsedData)>;
      using handlePreparsedDataMap_t = std::map<::HANDLE, parsedDataPtr>;

      handlePreparsedDataMap_t const handlePreparsedDataMap
      {
        [&openDeviceHandles]
        {
          handlePreparsedDataMap_t dataMap;
          for(auto const & elem : openDeviceHandles)
          {
            auto const & hnd{ elem.first };
            ::PHIDP_PREPARSED_DATA preparsedData{};
            if(::HidD_GetPreparsedData(hnd, &preparsedData))
            {
              dataMap.emplace(hnd, parsedDataPtr(preparsedData, &::HidD_FreePreparsedData));
            }
          }
          return dataMap;
        }()
      };

      ::HIDP_CAPS hidCaps{};
      using handleCapsMap_t = std::map<::HANDLE, ::HIDP_CAPS>;
      handleCapsMap_t const devPathCapsMap
      {
        [&handlePreparsedDataMap]
        {
          handleCapsMap_t devCaps;
          ::HIDP_CAPS hidCaps;
          for(auto const &[hnd, preParsedData] : handlePreparsedDataMap)
          {
            if(::HidP_GetCaps(preParsedData.get(), &hidCaps) == HIDP_STATUS_SUCCESS)
            {
              devCaps.emplace(hnd, hidCaps);
            }
          }
          return devCaps;
        }()
      };

      assert(handleAttributesMap.size() == handlePreparsedDataMap.size() &&
        handlePreparsedDataMap.size() == devPathCapsMap.size() &&
        "Number of device attributes, device preparsed data, and device capabilities should be equal");

      installedDeviceInfoList_t installedDeviceInfoList;
      installedDeviceInfoList.reserve(devPathCapsMap.size());
      for(auto const[hnd, attribs] : handleAttributesMap)
      {
        hidDeviceInfo const devInfo{ openDeviceHandles.at(hnd), hndManufacturerMap.at(hnd), attribs, devPathCapsMap.at(hnd) };
        installedDeviceInfoList.emplace_back(devInfo);
      }
      return installedDeviceInfoList;
    }
  }
  catch(...)
  {
    assert(false && "Unknown exception thrown in GetlInstalledDevicesInfo()\n");
  }
  return {};

}

// ################### end interface #########################################

namespace
{
  ::GUID GetHidGuid() noexcept
  {
    ::GUID guid;
    ::HidD_GetHidGuid(&guid);

    return guid;
  }

  std::wstring GetDevicePath(::HDEVINFO hDevInfo, ::SP_DEVICE_INTERFACE_DATA & diData) noexcept
  {
    try
    {
      DWORD requiredSize{};
      ::SetupDiGetDeviceInterfaceDetail(hDevInfo, &diData, nullptr, 0, &requiredSize, nullptr);

      std::unique_ptr<::BYTE[]> const diDetailData{ new ::BYTE[requiredSize] };

      auto const diDetailDataPtr{ reinterpret_cast<::PSP_DEVICE_INTERFACE_DETAIL_DATA>(diDetailData.get()) };
      diDetailDataPtr->cbSize = sizeof ::SP_DEVICE_INTERFACE_DETAIL_DATA;

      if(::SetupDiGetDeviceInterfaceDetail(hDevInfo, &diData, diDetailDataPtr, requiredSize, nullptr, nullptr))
      {
        return diDetailDataPtr->DevicePath;
      }
    }
    catch(...)
    {
      assert(false && "Unknown exception thrown in GetDevicePath()\n");
    }
    return {};

  }
}