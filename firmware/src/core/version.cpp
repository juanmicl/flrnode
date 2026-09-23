// Placeholder translation unit so the `native` env has a buildable host
// program until real core sources and Unity tests land (see Tasks 2+).
// The main() is weak: the Unity test runner's main overrides it once tests
// exist, so `pio run` and `pio test -e native` both work. Delete this file
// when core sources provide real content.
__attribute__((weak)) int main() { return 0; }
