#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct {
    int16_t  someField[24];
    int32_t  someInt;
    int16_t  productName[256];
    int16_t  firmwareName[256];
    int32_t  partitionCount;
    int32_t  partitionsListStart;
    int32_t  someIntFields1[5];
    int16_t  productName2[50];
    int16_t  someIntFields2[6];
    int16_t  someIntFields3[2];
} PacHeader;

typedef struct {
    uint32_t length;
    int16_t  partitionName[256];
    int16_t  fileName[512];
    uint32_t partitionSize;
    int32_t  someFileds1[2];
    uint32_t partitionAddrInPac;
    int32_t  someFileds2[3];
} PartitionHeader;

// Converts UTF-16LE / wide string field to a standard null-terminated ASCII string
void getString(const int16_t* src, char* dest, size_t max_len) {
    size_t i = 0;
    while (src[i] != 0 && i < max_len - 1) {
        dest[i] = (char)(src[i] & 0xFF);
        i++;
    }
    dest[i] = '\0';
}

// Helper to exit gracefully with an error message
void fatal_error(const char* msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: capextractor <firmware name>.pac\n");
        return EXIT_FAILURE;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd == -1) fatal_error("Error opening input file");

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < sizeof(PacHeader)) {
        fprintf(stderr, "File %s is not a valid firmware image.\n", argv[1]);
        close(fd);
        return EXIT_FAILURE;
    }

    PacHeader pacHeader;
    if (read(fd, &pacHeader, sizeof(PacHeader)) != sizeof(PacHeader)) {
        fprintf(stderr, "Error parsing PAC header.\n");
        close(fd);
        return EXIT_FAILURE;
    }

    char fwName[256];
    getString(pacHeader.firmwareName, fwName, sizeof(fwName));
    printf("Firmware name: %s\n", fwName);

    // Allocate array to track partition meta-headers
    PartitionHeader** partHeaders = malloc(sizeof(PartitionHeader*) * pacHeader.partitionCount);
    if (!partHeaders) fatal_error("Memory allocation failed");

    uint32_t curPos = pacHeader.partitionsListStart;

    // Phase 1: Parse and store partition metadata headers
    for (int i = 0; i < pacHeader.partitionCount; i++) {
        uint32_t length;
        lseek(fd, curPos, SEEK_SET);
        
        if (read(fd, &length, sizeof(uint32_t)) != sizeof(uint32_t)) {
            fatal_error("Error reading partition length");
        }

        partHeaders[i] = malloc(length);
        if (!partHeaders[i]) fatal_error("Memory allocation failed");

        lseek(fd, curPos, SEEK_SET);
        if (read(fd, partHeaders[i], length) != length) {
            fatal_error("Error reading partition header");
        }

        char partName[256], fileName[512];
        getString(partHeaders[i]->partitionName, partName, sizeof(partName));
        getString(partHeaders[i]->fileName, fileName, sizeof(fileName));
        
        printf("Partition: %s -> File: %s (%u bytes)\n", partName, fileName, partHeaders[i]->partitionSize);
        curPos += length;
    }

    // Phase 2: Extract data payloads to disk
    char ioBuffer[4096]; // Increased buffer size to 4KB for significantly faster sequential extraction
    for (int i = 0; i < pacHeader.partitionCount; i++) {
        if (partHeaders[i]->partitionSize == 0) {
            free(partHeaders[i]);
            continue;
        }

        char outFileName[512];
        getString(partHeaders[i]->fileName, outFileName, sizeof(outFileName));
        
        int fd_out = open(outFileName, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd_out == -1) {
            perror("Failed to create output file");
            free(partHeaders[i]);
            continue;
        }

        printf("Extracting %s... ", outFileName);
        fflush(stdout);

        lseek(fd, partHeaders[i]->partitionAddrInPac, SEEK_SET);
        uint32_t bytesLeft = partHeaders[i]->partitionSize;

        while (bytesLeft > 0) {
            uint32_t chunk = (bytesLeft > sizeof(ioBuffer)) ? sizeof(ioBuffer) : bytesLeft;
            
            if (read(fd, ioBuffer, chunk) != chunk || write(fd_out, ioBuffer, chunk) != chunk) {
                fprintf(stderr, "\nExtraction error on chunk payload.\n");
                exit(EXIT_FAILURE);
            }
            bytesLeft -= chunk;
            
            // Print clear extraction metrics
            double pct = 100.0 * (partHeaders[i]->partitionSize - bytesLeft) / partHeaders[i]->partitionSize;
            printf("\rExtracting %s... %.1f%%", outFileName, pct);
            fflush(stdout);
        }
        printf("\n");
        
        close(fd_out);
        free(partHeaders[i]);
    }

    free(partHeaders);
    close(fd);
    return EXIT_SUCCESS;
}
