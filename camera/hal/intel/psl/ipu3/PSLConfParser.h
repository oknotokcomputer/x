/*
 * Copyright (C) 2014-2018 Intel Corporation
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

#ifndef _CAMERA3_IPU3CONFPARSER_H_
#define _CAMERA3_IPU3CONFPARSER_H_

#include <vector>
#include <string>
#include "PlatformData.h"
#include "IPU3CameraCapInfo.h"
#include "MediaCtlPipeConfig.h"

#define VIDEO_DEV_NAME "Unimplemented"
#define ANDROID_CONTROL_CAPTURE_INTENT_START 0x40000000
#define CAMERA_TEMPLATE_COUNT (ANDROID_CONTROL_CAPTURE_INTENT_MANUAL + 1)

namespace android {
namespace camera2 {

class PSLConfParser {
public:
    static PSLConfParser *getInstance(std::string &xmlConfigName, const std::vector<SensorDriverDescriptor>& sensorNames);
    static void deleteInstance();
    static std::string getSensorMediaDevice();
    static std::string getImguMediaDevice();

    CameraCapInfo *getCameraCapInfo(int cameraId);
    camera_metadata_t *constructDefaultMetadata(int cameraId, int reqTemplate);

    static const char *getSensorMediaDeviceName() { return "ipu3-cio2"; }
    static const char *getImguEntityMediaDevice() { return "ipu3-imgu"; }

    static std::string getSensorMediaDevicePath();
    static std::string getMediaDeviceByName(std::string deviceName);

// disable copy constructor and assignment operator
private:
    PSLConfParser(PSLConfParser const&);
    PSLConfParser& operator=(PSLConfParser const&);

private:
    static PSLConfParser *sInstance; // the singleton instance

    PSLConfParser(std::string& xmlName, const std::vector<SensorDriverDescriptor>& sensorNames);
    ~PSLConfParser();

    static const int mBufSize = 1*1024;  // For xml file

    static std::string mImguMediaDevice;
    static std::string mSensorMediaDevice;

    std::string mXmlFileName;
    std::vector<SensorDriverDescriptor> mDetectedSensors;
    std::vector<CameraCapInfo *> mCaps;
    std::vector<camera_metadata_t *> mDefaultRequests;

    enum DataField {
        FIELD_INVALID = 0,
        FIELD_HAL_TUNING_IPU3,
        FIELD_SENSOR_INFO_IPU3,
        FIELD_MEDIACTL_ELEMENTS_IPU3,
        FIELD_MEDIACTL_CONFIG_IPU3
    } mCurrentDataField;
    unsigned mSensorIndex;
    MediaCtlConfig mMediaCtlCamConfig;     // one-selected camera pipe config.
    bool mUseProfile;   /**< internal variable to disable parsing of profiles of
                             sensor not found at run time */

    static void startElement(void *userData, const char *name, const char **atts);
    static void endElement(void *userData, const char *name);
    void checkField(const char *name, const char **atts);
    void getDataFromXmlFile(void);
    bool isSensorPresent(const std::string &sensorName);
    uint8_t selectAfMode(const camera_metadata_t *staticMeta, int reqTemplate);
    status_t addCamera(int cameraId, const std::string &sensorName);
    void handleHALTuning(const char *name, const char **atts);
    void handleSensorInfo(const char *name, const char **atts);
    void handleMediaCtlElements(const char *name, const char **atts);
    void getPSLDataFromXmlFile(void);
    int convertXmlData(void * dest, int destMaxNum, const char * src, int type);
    void getGraphConfigFromXmlFile();

    int getStreamFormatAsValue(const char* format);
    int getSelectionTargetAsValue(const char* target);
    int getControlIdAsValue(const char* format);
    int getIsysNodeNameAsValue(const char* isysNodeName);

    int readNvmData();
    void dumpHalTuningSection(int cameraId);
    void dumpSensorInfoSection(int cameraId);
    void dumpMediaCtlElementsSection(int cameraId);
    void dump(void);

    std::vector<std::string> mElementNames;
};

} // namespace camera2
} // namespace android
#endif
