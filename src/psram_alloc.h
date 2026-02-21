#include <ArduinoJson.h>

void *psram_calloc(size_t n, size_t size);  
void psram_free(void *ptr);

// PSRAM allocator for ArduinoJson - use as a global singleton
// SIMPLIFIED: Only use PSRAM, no complex fallbacks that could cause corruption
class SpiRamAllocator : public ArduinoJson::Allocator {
public:
  static SpiRamAllocator* instance() {
    static SpiRamAllocator allocator;
    return &allocator;
  }

  void* allocate(size_t size) override {
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
      Serial.printf(">> ERROR: PSRAM allocation failed for %d bytes\n", size);
    }
    return ptr;
  }

  void deallocate(void* pointer) override {
    if (pointer) {
      heap_caps_free(pointer);
    }
  }

  void* reallocate(void* ptr, size_t new_size) override {
    if (!ptr) {
      return allocate(new_size);
    }
    if (new_size == 0) {
      deallocate(ptr);
      return nullptr;
    }
    
    // Simple PSRAM realloc - no complex fallbacks
    void* new_ptr = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!new_ptr) {
      Serial.printf(">> ERROR: PSRAM realloc failed for %d bytes\n", new_size);
    }
    return new_ptr;
  }

private:
  SpiRamAllocator() = default;
};
