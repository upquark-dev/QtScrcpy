extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/log.h>
}
#include <cstdio>
int main() {
    void *opq = nullptr;
    const AVInputFormat *f;
    int n = 0;
    printf("=== available demuxers ===\n");
    while ((f = av_demuxer_iterate(&opq))) {
        printf("%s (%s)\n", f->name, f->long_name ? f->long_name : "");
        n++;
    }
    printf("total=%d\n", n);
    const AVOutputFormat *of;
    void *opq2 = nullptr;
    printf("=== available muxers ===\n");
    while ((of = av_muxer_iterate(&opq2))) {
        printf("%s\n", of->name);
    }
    return 0;
}
