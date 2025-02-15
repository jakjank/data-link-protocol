#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "frame.h"
#include "serial_port.h"


// Global variables
#define BUFSIZE 1024


int bufferSize = 0; 
unsigned char buffer[BUFSIZE] = {0};

int processedBufferSize = 0;
unsigned char processedBuffer[BUFSIZE] = {0};

void stuffing() {
    processedBufferSize = 0;

    for (int i = 0; i < bufferSize; i++) {
        uint8_t byte = buffer[i];
        
        if (byte == FLAG) {
            // Replace 0x7E with 0x7D 0x5E
            processedBuffer[processedBufferSize++] = ESCAPE;
            processedBuffer[processedBufferSize++] = byte ^ 0x20;
        } else if (byte == ESCAPE) {
            // Replace 0x7D with 0x7D 0x5D
            processedBuffer[processedBufferSize++] = ESCAPE;
            processedBuffer[processedBufferSize++] = byte ^ 0x20;
        } else {
            // Regular byte, add to output
            processedBuffer[processedBufferSize++] = byte;
        }
    }
}

void destuffing() {
    processedBufferSize = 0;
    for (int i = 0; i < bufferSize; i++) {
        uint8_t byte = buffer[i];
        
        if (byte == ESCAPE) {
            // Escape sequence detected, XOR the next byte with 0x20
            i++;  // Move to next byte
            if (i < bufferSize) {
                processedBuffer[processedBufferSize++] = buffer[i] ^ 0x20;
            }
        } else {
            // Regular byte, add to output
            processedBuffer[processedBufferSize++] = byte;
        }
    }
}

uint8_t calculate_bcc(const unsigned char* bccbuff, int start, int len) {
    uint8_t bcc = 0;
    for (int i = start; i < start + len; i++) {
        bcc ^= bccbuff[i];  // XOR each byte
    }
    return bcc;
}

int bccfailed = 0;
int populateFrame(Frame* frame){
    bccfailed = 0;
    destuffing();

    frame->address = processedBuffer[0];
    frame->control = processedBuffer[1];
    frame->bcc1 = processedBuffer[2];

    if (frame->bcc1 != (frame->address^frame->control)){
        printf("BCC1 failed!\n");
        printf("CALCULATED BCC1: %02x, %02x - %02x\n", frame->address^frame->control, frame->address, frame->control);
        printf("RECIVED BCC1: %02x\n", frame->bcc1);

        bccfailed=1;
        return -1; //BCC1 failed!
    } 
        
    
    if (processedBufferSize == 3) return processedBufferSize; //Control frame

    frame->infoFrame.dataSize = processedBufferSize - 4; 
    
    uint8_t bcc = calculate_bcc(processedBuffer, 3, frame->infoFrame.dataSize);

    if (bcc != (uint8_t)processedBuffer[processedBufferSize - 1]){
        printf("BCC2 failed!\n");
        printf("CALCULATED BCC2: %02x\n", bcc);
        printf("EXPECTED BCC2: %02x\n", (uint8_t)processedBuffer[processedBufferSize - 1]);
        bccfailed=1;
        return -1; //BCC2 failed!
    }
    
    for (int i = 0; i < frame->infoFrame.dataSize; i++) {
        frame->infoFrame.data[i] = processedBuffer[i + 3];
    }

    bccfailed=0;
    return processedBufferSize;
}

int start_found = 0;
int read_frame(Frame* frame) {
    char byte;

    int readcount = readByte(&byte);
    if(readcount == 0) return 0; // No byte read

    //printf("Read byte: %02x\n", byte);

    if(bccfailed == 1){
        if(byte == 0x03 || byte == 0x01){
            printf("######################################################\n######################################################\n######################################################\n######################################################\n######################################################\n######################################################\n");
            printf("VERY LIKELY FLAG DESYNC, fixing\n");
            memset(buffer, 0, sizeof(buffer));
            memset(processedBuffer, 0, sizeof(processedBuffer));
            start_found = 1;
            bufferSize = 0;

            bccfailed = 0;

            buffer[bufferSize++] = byte;
            printf("Read byte: %02x\n", byte);
            return 1;  // Byte read
        }
        bccfailed = 0;
    }

    if (byte == FLAG) {
        if (!start_found) {
            
            // Start of frame
            memset(buffer, 0, sizeof(buffer));
            memset(processedBuffer, 0, sizeof(processedBuffer));
            start_found = 1;
            bufferSize = 0;
        } else {
            if(bufferSize < 2){
                return 1;
            }
            // End of frame
            start_found = 0;
            return populateFrame(frame);
        }
    } else if (start_found) {
        
        buffer[bufferSize++] = byte;
        if (bufferSize >= sizeof(buffer)) {
            printf("Frame too large!\n");
            return -2;  // Error: frame exceeds buffer size
        }
        return 1;  // Byte read
    }  

    return 0;
}

int writtenFrames = 0;
int write_frame(Frame frame) {
    bufferSize = 0;
    writtenFrames++;

    buffer[bufferSize++] = 0; // Start flag
    buffer[bufferSize++] = frame.address;
    buffer[bufferSize++] = frame.control;
    buffer[bufferSize++] = frame.bcc1;

    //Add data if it's an I frame
    if (frame.infoFrame.dataSize > 0) {
        memcpy(buffer + bufferSize, frame.infoFrame.data, frame.infoFrame.dataSize);
        bufferSize += frame.infoFrame.dataSize;
        buffer[bufferSize++] = frame.infoFrame.bcc2;
    }

    buffer[bufferSize++] = 0; // End flag
    stuffing();

    // Add flags after stuffing
    processedBuffer[0] = FLAG;
    processedBuffer[processedBufferSize - 1] = FLAG;

    int temp = writeBytes(processedBuffer, processedBufferSize);

    printf("BCC2 %02x\n", frame.infoFrame.bcc2);
    printf("SENT!\n");
    return temp;
}

Frame create_frame(uint8_t type, uint8_t address, const unsigned char* packet, int packetSize) {
    Frame frame = {0};
    if (packetSize > sizeof(frame.infoFrame.data)){
        printf("[CRITICAL ERROR] Packet too large!\n");
        return frame;
    }
    
    frame.address = address;
    frame.control = type;
    frame.bcc1 = frame.address ^ frame.control;
    
    printf("Address: %02x\n", frame.address);
    printf("Control: %02x\n", frame.control);
    printf("BCC1: %02x\n", frame.bcc1);

    if (packetSize > 0) {
        frame.infoFrame.dataSize = packetSize;
        memcpy(frame.infoFrame.data, packet, packetSize);
        frame.infoFrame.bcc2 = calculate_bcc(packet, 0, packetSize);
    }    
    
    return frame;
}
