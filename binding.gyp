{
  "variables": {
    "openssl_fips%": ""
  },
  
  "targets": [
    {
      "target_name": "addon_iec61850",
      "sources": [
        "src/addon.cc",
        "src/mms_client.cc",
        "src/goose_subscriber.cc"
      ],      
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "include/libiec61850",               
        "src",        
      ],
      "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS"],
      "cflags": ["-Wall", "-Wno-unused-parameter"],
      "cflags_cc": ["-Wall", "-Wno-unused-parameter", "-std=c++17", "-fexceptions"],
      "conditions": [
        ["OS=='mac' and target_arch=='arm64'", {
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "MACOSX_DEPLOYMENT_TARGET": "11.0",
            "ARCHS": ["arm64"],
            "OTHER_CFLAGS": ["-Wall", "-Wno-unused-parameter"],
            "OTHER_CPLUSPLUSFLAGS": ["-Wall", "-Wno-unused-parameter", "-std=c++17", "-fexceptions"]
          },
          "libraries": [
            "<(module_root_dir)/lib/build/libiec61850-macos-latest-arm64.a"
          ]
        }],
        ["OS=='mac' and target_arch=='x64'", {
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "MACOSX_DEPLOYMENT_TARGET": "11.0",
            "ARCHS": ["x64"],
            "OTHER_CFLAGS": ["-Wall", "-Wno-unused-parameter"],
            "OTHER_CPLUSPLUSFLAGS": ["-Wall", "-Wno-unused-parameter", "-std=c++17", "-fexceptions"]
          },
          "libraries": [
            "<(module_root_dir)/lib/build/libiec61850-macos-latest-x64.a"
          ]
        }],
        ["OS=='linux' and target_arch=='x64'", {
          "cflags": ["-fPIC"],
          "cflags_cc": ["-fPIC"],
          "libraries": [
            "<(module_root_dir)/lib/build/libiec61850-Ubuntu-latest-x64.a",
            "-lpthread"
          ]
        }],
        ["OS=='linux' and target_arch=='arm64'", {
          "cflags": ["-fPIC", "-march=armv8-a"],
          "cflags_cc": ["-fPIC", "-march=armv8-a"],
          "libraries": [
            "<(module_root_dir)/lib/build/libiec61850-ubuntu-latest-arm64.a",
            "-lpthread"
          ]
        }],
        ["OS=='linux' and target_arch=='arm'", {
          "cflags": ["-fPIC", "-march=armv7-a", "-mfpu=vfp", "-mfloat-abi=hard"],
          "cflags_cc": ["-fPIC", "-march=armv7-a", "-mfpu=vfp", "-mfloat-abi=hard"],
          "libraries": [
            "<(module_root_dir)/lib/build/libiec61850-ubuntu-latest-arm.a",
            "-lpthread"
          ]
        }],
        ["OS=='win' and target_arch=='x64'", {
          "msvs_settings": {
            "VCCLCompilerTool": {
              "ExceptionHandling": 1,
              "AdditionalOptions": ["/std:c++17"]
            }
          },
          "libraries": [
            "<(module_root_dir)/lib/build/libiec61850-windows-latest-x64.lib",
            "<(module_root_dir)/lib/build/hal.lib",
            "<(module_root_dir)/third_party/mbedtls/library/Release/mbedcrypto.lib",
            "<(module_root_dir)/third_party/mbedtls/library/Release/mbedx509.lib",
            "<(module_root_dir)/third_party/mbedtls/library/Release/mbedtls.lib",
            "<(module_root_dir)/third_party/winpcap/Lib/x64/wpcap.lib",
            "<(module_root_dir)/third_party/winpcap/Lib/x64/Packet.lib",
            "ws2_32.lib",
            "iphlpapi.lib",
            "bcrypt.lib",
            "crypt32.lib",
            "advapi32.lib"
          ]
        }]
      ]
    }
  ]
}