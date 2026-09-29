def pytest_configure(config):
    config.addinivalue_line("markers", "hardware: needs real bridges attached")
