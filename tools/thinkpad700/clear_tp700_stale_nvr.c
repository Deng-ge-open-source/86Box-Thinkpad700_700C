#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static int
clear_byte(const char *path, long offset, unsigned char expected_mask,
           const char *name)
{
    FILE *fp = fopen(path, "r+b");
    int value;

    if (!fp) {
        perror(path);
        return 1;
    }
    if (fseek(fp, offset, SEEK_SET) != 0 || (value = fgetc(fp)) == EOF) {
        fprintf(stderr, "%s: cannot read %s byte at 0x%lx\n",
                path, name, offset);
        fclose(fp);
        return 1;
    }
    if ((value & ~expected_mask) != 0) {
        fprintf(stderr,
                "%s: refusing unexpected %s value 0x%02x at 0x%lx\n",
                path, name, value, offset);
        fclose(fp);
        return 1;
    }
    if (fseek(fp, offset, SEEK_SET) != 0 || fputc(0, fp) == EOF ||
        fflush(fp) != 0) {
        perror(path);
        fclose(fp);
        return 1;
    }
    if (fclose(fp) != 0) {
        perror(path);
        return 1;
    }
    printf("cleared %s: 0x%02x -> 0x00\n", name, value);
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s primary.nvr secondary.nvr\n", argv[0]);
        return 2;
    }

    /*
     * Primary 0Eh bit 4 and secondary 0192h bits 1/6 are the fault records
     * left by the now-fixed oversized RAM remap and floating E3h status port.
     * Validate that no unrelated bits are present before clearing either.
     */
    if (clear_byte(argv[1], 0x0e, 0x10, "CMOS diagnostic status"))
        return 1;
    if (clear_byte(argv[2], 0x192, 0x42, "POST memory-fault record"))
        return 1;
    return 0;
}
