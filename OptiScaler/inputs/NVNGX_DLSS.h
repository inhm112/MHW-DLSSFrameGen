#pragma once

template <typename FeatureType> struct ContextData
{
    std::unique_ptr<FeatureType> feature;
    NVSDK_NGX_Parameter* createParams = nullptr;
    bool createParamsOwned = false;
    int changeBackendCounter = 0;
    bool terminalFailure = false;
};
