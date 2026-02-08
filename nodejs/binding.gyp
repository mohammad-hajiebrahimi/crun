{
  "targets": [
    {
      "target_name": "crun_binding",
      "sources": [
        "src/crun_binding.c"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "/usr/local/include",
        "../",
        "../src",
        "../src/libcrun",
        "../libocispec/src"
      ],
      "libraries": [
        "-L/usr/local/lib",
        "-lcrun",
        "-Wl,-rpath,/usr/local/lib",
        "<!@(pkg-config --libs yajl)",
        "<!@(pkg-config --libs libseccomp)",
        "<!@(pkg-config --libs libcap)",
        "<!@(pkg-config --libs libsystemd)",
        "-lpthread",
        "-ldl",
        "-lm"
      ],
      "cflags": [
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
        "-Wno-unused-function",
        "-fPIC",
        "<!@(pkg-config --cflags yajl)",
        "<!@(pkg-config --cflags libseccomp)",
        "<!@(pkg-config --cflags libcap)"
      ],
      "cflags!": ["-fno-exceptions"],
      "defines": [
        "NAPI_VERSION=8",
        "NAPI_DISABLE_CPP_EXCEPTIONS",
        "HAVE_CONFIG_H"
      ],
      "conditions": [
        ["OS=='linux'", {
          "defines": ["__linux__"]
        }]
      ]
    }
  ]
}