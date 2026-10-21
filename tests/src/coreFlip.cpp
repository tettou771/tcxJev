// Deliberately separate from the addon encoder TU: these are core's external symbols.
#include <TrussC.h>
#include <string>
void setCoreImageFlip(int value) { stbi_flip_vertically_on_write(value); }
std::string corePng(const tc::Pixels& p) {
    std::string bytes;
    stbi_write_png_to_func([](void* out, void* data, int size) {
        static_cast<std::string*>(out)->append(static_cast<char*>(data), size_t(size));
    }, &bytes, p.getWidth(), p.getHeight(), p.getChannels(), p.getData(), p.getWidth() * p.getChannels());
    return bytes;
}
