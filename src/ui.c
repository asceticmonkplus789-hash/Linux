/* 仅负责显示。检测终端后使用中文和单行进度；管道/文件保持原有纯文本格式。 */
#include "ui.h"
#include <sys/ioctl.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int pretty, color, active_line;
static int64_t last_frame;

void ui_init(void)
{
    const char *term=getenv("TERM");
    pretty=isatty(STDOUT_FILENO) && term && *term && strcmp(term,"dumb");
    color=pretty && !getenv("NO_COLOR");
}

int ui_enabled(void) { return pretty; }

static void size_text(double bytes, char *out, size_t len)
{
    const char *units[]={"B","KiB","MiB","GiB","TiB","PiB","EiB"};
    size_t i=0;
    while (bytes>=1024.0 && i<6) { bytes/=1024.0; i++; }
    snprintf(out,len,"%.2f %s",bytes,units[i]);
}

static double percentage(off_t value, off_t size)
{
    return size ? 100.0*(double)value/(double)size : 100.0;
}

static void title(const char *label, const char *code)
{
    if (color) printf("\033[%sm",code);
    printf("\n  %s\n",label);
    if (color) fputs("\033[0m",stdout);
    puts("  ------------------------------------------------");
}

void ui_end_progress(void)
{
    if (active_line) { putchar('\n'); fflush(stdout); active_line=0; }
}

void ui_error(const char *message)
{
    ui_end_progress();
    fprintf(stderr,pretty ? "[错误] %s\n" : "error: %s\n",message);
}

void ui_created(const char *path, off_t size)
{
    if (!pretty) { printf("created: %s, logical_bytes=%jd\n",path,(intmax_t)size); return; }
    char human[32]; size_text((double)size,human,sizeof(human));
    title("[已创建] 空洞文件","1;36");
    printf("  文件路径：%s\n  逻辑大小：%s（%jd 字节）\n",path,human,(intmax_t)size);
    puts("  使用 status 命令查看实际占用和空洞比例。\n");
}

void ui_written(const char *path, off_t offset, size_t bytes)
{
    if (!pretty) { printf("written: offset=%jd bytes=%zu\n",(intmax_t)offset,bytes); return; }
    title("[已写入] 定点修改完成","1;32");
    printf("  文件路径：%s\n  起始偏移：%jd 字节\n  写入长度：%zu 字节\n\n",path,(intmax_t)offset,bytes);
}

void ui_status(const char *path, off_t size, uintmax_t blocks,
               uintmax_t allocated, double saving, int supported, off_t holes)
{
    if (!pretty) {
        printf("logical_bytes=%jd\nallocated_blocks_512=%ju\nallocated_bytes=%ju\n",
               (intmax_t)size,blocks,allocated);
        printf("space_saving_percent=%.2f (block-accounting estimate)\n",saving);
        if (supported) printf("hole_bytes=%jd\nhole_percent=%.2f (filesystem extent report)\n",
                              (intmax_t)holes,size ? percentage(holes,size) : 0.0);
        else puts("hole_percent=unavailable (filesystem does not report extents)");
        return;
    }
    char logical[32], disk[32], hole[32];
    size_text((double)size,logical,sizeof(logical));
    size_text((double)allocated,disk,sizeof(disk));
    size_text((double)holes,hole,sizeof(hole));
    title("[文件状态] 空洞与磁盘占用","1;36");
    printf("  文件路径：%s\n\n",path);
    printf("  逻辑大小：%s（%jd 字节）\n",logical,(intmax_t)size);
    printf("  实际占用：%s（%ju 字节）\n",disk,allocated);
    printf("  分配块数：%ju（每块 512 字节）\n",blocks);
    printf("  空间节省：%.2f%%（按已分配块估算）\n",saving);
    if (supported) {
        printf("  空洞大小：%s（%jd 字节）\n",hole,(intmax_t)holes);
        printf("  空洞占比：%.2f%%（文件系统报告）\n",size ? percentage(holes,size) : 0.0);
    } else puts("  空洞占比：当前文件系统不支持查询");
    putchar('\n');
}

void ui_start(const char *path, off_t offset, off_t size)
{
    if (!pretty) {
        printf("resume_offset=%jd total_bytes=%jd\n",(intmax_t)offset,(intmax_t)size);
    } else {
        char total[32]; size_text((double)size,total,sizeof(total));
        title(offset==size ? "[检查文件] 已有完整断点" : offset ? "[断点续传] 继续下载" : "[开始下载] HTTP 空洞文件下载器","1;36");
        printf("  目标文件：%s\n  文件大小：%s（%jd 字节）\n",path,total,(intmax_t)size);
        printf("  起始偏移：%jd 字节\n",(intmax_t)offset);
        if (offset<size) puts("  提示：按 Ctrl+C 暂停；再次运行原命令即可续传。\n");
    }
    fflush(stdout);
}

void ui_progress(off_t written, off_t size, off_t saved)
{
    if (!pretty) {
        printf("progress=%jd/%jd (%.1f%%) saved_offset=%jd\n",(intmax_t)written,(intmax_t)size,
               percentage(written,size),(intmax_t)saved);
        fflush(stdout);
        return;
    }
    /* 只限制界面刷新频率，不参与写入、同步或断点保存决策。 */
    struct timespec now;
    int64_t ms=0;
    if (!clock_gettime(CLOCK_MONOTONIC,&now)) ms=(int64_t)now.tv_sec*1000+now.tv_nsec/1000000;
    if (ms && last_frame && ms-last_frame<100 && written!=size) return;
    last_frame=ms;
    struct winsize window;
    unsigned columns=80;
    if (!ioctl(STDOUT_FILENO,TIOCGWINSZ,&window) && window.ws_col) columns=window.ws_col;
    char received[32], total[32], committed[32];
    size_text((double)written,received,sizeof(received));
    size_text((double)size,total,sizeof(total));
    size_text((double)saved,committed,sizeof(committed));
    double percent=percentage(written,size);
    fputs("\r\033[2K",stdout);
    if (columns>=90) {
        char bar[17];
        unsigned fill=(unsigned)(percent*16.0/100.0);
        for (unsigned i=0;i<16;i++) bar[i]=i<fill ? '#' : '-';
        bar[16]=0;
        printf("  [%s] %5.1f%% | 已写 %s / %s | 已存 %s",bar,percent,received,total,committed);
    } else if (columns>=64) printf("  进度 %5.1f%% | 已写 %s | 已存 %s",percent,received,committed);
    else if (columns>=20) printf("  进度 %5.1f%%",percent);
    else printf("%.1f%%",percent);
    active_line=1;
    fflush(stdout);
}

void ui_paused(off_t saved, off_t size)
{
    ui_end_progress();
    if (!pretty) { printf("paused: completed_offset=%jd; rerun the same download command\n",(intmax_t)saved); return; }
    char total[32]; size_text((double)size,total,sizeof(total));
    title("[已暂停] 断点已保存","1;33");
    printf("  文件大小：%s\n  保存偏移：%jd 字节\n  完成进度：%.1f%%\n",total,(intmax_t)saved,percentage(saved,size));
    puts("\n  再次运行原命令，即可从上述位置继续。\n");
}

void ui_complete(off_t saved, off_t size)
{
    ui_end_progress();
    if (!pretty) {
        printf("complete: saved_offset=%jd total_bytes=%jd\n"
               "Compare source/destination SHA-256 separately for content verification.\n",(intmax_t)saved,(intmax_t)size);
        return;
    }
    char total[32]; size_text((double)size,total,sizeof(total));
    title("[已完成] 文件与最终断点已保存","1;32");
    printf("  文件大小：%s\n  保存偏移：%jd 字节\n  完成进度：100.0%%\n",total,(intmax_t)saved);
    puts("\n  内容一致性请另行对比源文件与下载文件的 SHA-256。\n");
}

void ui_help(void)
{
    title("SparseGet | 空洞文件与断点续传","1;36");
    puts("  创建文件  sparseget create 文件 大小\n"
         "            大小至少 128K；K/M/G 按 1024 进位\n"
         "  定点写入  sparseget write 文件 偏移 文本\n"
         "  查看状态  sparseget status 文件\n"
         "  下载续传  sparseget download URL 文件 [--delay-ms 毫秒]\n\n"
         "  重置演示  sparseget reset 文件             先预览两个路径\n"
         "            sparseget reset 文件 --confirm   移到备份后可从头下载\n"
         "  恢复备份  sparseget restore 文件 备份目录   拒绝覆盖已有文件\n\n"
         "  Ctrl+C 保存断点并暂停；再次执行原命令续传。\n"
         "  已写入与已保存进度不同；约每 1 MiB 保存一次，正常退出前补存。\n"
         "  支持明文 HTTP、HEAD、Content-Length、强 ETag 和 Range。\n"
         "  不支持 HTTPS、重定向、压缩和分块传输。\n"
         "  ETag 识别远端版本，FNV 检查本地前缀；SHA-256 需另行对比。\n"
         "  写入或下载要求文件只有一个硬链接；已有文件不会被直接覆盖。\n"
         "  退出码：0 成功 / 1 错误 / 2 用法错误 / 130 中断。\n"
         "  输出到文件或管道时自动使用纯文本日志。\n");
}
