# Protocol parser fuzzing

`protocol_fuzz.cc` contains the bounded libFuzzer entry point; `corpus/` holds
binary seed inputs. Build, execution and failure-reproduction commands are in
[FUZZING.md](../docs/FUZZING.md). Evolved corpora and crash artifacts belong in
the ignored build directory, not among checked-in seeds without review.
