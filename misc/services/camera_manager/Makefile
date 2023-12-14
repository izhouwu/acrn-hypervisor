CFLAGS = -fPIC

CAMERA_MANAGER = camera_manager.bin
CAMERA_CLIENT = camera_client.so
CAMERA_CONFIG = camera_config.so
CAMERA_CONFIG_TEST = camera_config_test.bin

all: $(CAMERA_MANAGER) $(CAMERA_CLIENT) $(CAMERA_CONFIG) $(CAMERA_CONFIG_TEST)

%.o: %.cpp
	$(CXX) $(CFLAGS) -c $< -o $@ -g

$(CAMERA_MANAGER): camera_data_provider.o camera_deserializer.o camera.o camera_manager.o camera_config.o
	$(CXX) $(CFLAGS) -o $@ $^ -g
$(CAMERA_CLIENT): camera_client.o camera_config.o
	$(CXX) $(CFLAGS) -shared -o $@ $^ -g
$(CAMERA_CONFIG): camera_config.o
	$(CXX) $(CFLAGS) -shared -o $@ $^ -g
$(CAMERA_CONFIG_TEST): camera_config_test.o
	$(CC) $(CFLAGS) -o $@ $^ camera_config.so -g

install: $(CAMERA_MANAGER)
	sudo cp *.bin /usr/bin/

clean:
	rm -f *.o *.so *.bin

.PHONY: clean install
