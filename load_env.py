# Reads .env (git-ignored, holds real WiFi/cellular/MQTT credentials) and
# injects each entry as a compiler #define, so telemetry_config.h's
# #ifndef-guarded placeholders get overridden at build time without any
# secret ever being written into a tracked file. Values that don't already
# have their own #define (see telemetry_config.h) are simply unused.
# No .env present -> no injected defines -> the placeholders in
# telemetry_config.h are used as-is, so a fresh clone still builds.

import os

Import("env")


def parse_env_file(path):
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            key = key.strip()
            value = value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
                value = value[1:-1]
            values[key] = value
    return values


env_path = os.path.join(env["PROJECT_DIR"], ".env")
env_values = parse_env_file(env_path)

defines = []
for key, value in env_values.items():
    if value.lstrip("-").isdigit():
        defines.append((key, value))
    else:
        escaped = value.replace("\\", "\\\\").replace('"', '\\"')
        defines.append((key, '\\"%s\\"' % escaped))

if defines:
    print("[load_env] injecting from .env: " + ", ".join(k for k, _ in defines))
    env.Append(CPPDEFINES=defines)
else:
    print("[load_env] no .env found -- using placeholder defaults in telemetry_config.h")
