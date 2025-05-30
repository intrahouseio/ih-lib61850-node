#include <napi.h>
#include "mms_client.h"
#include "goose_subscriber.h"

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    MmsClient::Init(env, exports);
    NodeGooseSubscriber::Init(env, exports);
    return exports;
}

NODE_API_MODULE(addon_iec61850, InitAll)