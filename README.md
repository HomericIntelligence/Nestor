# ProjectNestor

Research, ideation, and search for the HomericIntelligence distributed agent mesh.

Part of [Odysseus](https://github.com/HomericIntelligence/Odysseus) — the HomericIntelligence meta-repo.

## Role

```
User ↔ Odysseus ↔ Nestor ↔ Agamemnon ↔ agentic pipeline loop → completion
```

Nestor transforms raw ideas into researched briefs that Agamemnon can plan and execute.

## Building

Conan provides `cpp-httplib`, `nlohmann_json`, and `gtest`. Install
dependencies first — the CMake presets expect the Conan toolchain at
`build/debug/`:

```bash
just deps          # or: conan install . --output-folder=build/debug \
                   #         --profile=conan/profiles/debug --build=missing
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## License

MIT
