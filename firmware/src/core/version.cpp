// Stub main so `pio run -e native` links (no real main in src for native
// builds). Load-bearing since M1; keep until a src source provides a real
// main.
__attribute__((weak)) int main() { return 0; }
