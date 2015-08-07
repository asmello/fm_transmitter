#include "transmitter.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <exception>
#include <iostream>

// Base address for BCM2835 peripherals register memory
#define BCM2835_BASE 0x3F000000

// Base address for GPIO control registers (relative to the peripherals block)
#define GPIO_BASE 0x00200000

// Base address for GPC0 control registers (relative to the peripherals block)
#define CLK0_BASE 0x00101070

// Base address for GPC0 divisor control registers
// (relative to the peripherals block)
#define CLK0D_BASE 0x00101074

// Base address for the System Time Counter (relative to the peripherals block)
#define SYST_BASE 0x00003000

// Helper macros
#define ACCESS(base, offset) *(volatile unsigned int*)((int)base + offset)
#define ACCESS64(base, offset) *(volatile unsigned long long*)( \
  (int)base + offset)

Transmitter::Transmitter(double frequency)
{
    int memFd;

    // Get a file handle for the main physical memory
    if ((memFd = open("/dev/mem", O_RDWR | O_SYNC)) < 0) {
        std::cout << "Error: sudo privileges are required" << std::endl;
        throw std::exception();
    }

    // Map physical memory into process memory
    void *peripheralsMap = mmap(
      NULL, // any address in our space will do
      0x002FFFFF, // mapped block size (3MB)
      PROT_READ | PROT_WRITE, // enable read/write operations
      MAP_SHARED | MAP_LOCKED, // sync with other processes and lock into RAM
      memFd, // file handle for physical memory
      BCM2835_BASE // start of mapped block
    );

    close(memFd); // from now on just use the mapped memory

    if (peripheralsMap == MAP_FAILED) {
        std::cout << "Error: cannot obtain access to peripherals (mmap error)"
                  << std::endl;
        throw std::exception();
    }

    peripherals = (volatile unsigned*)peripheralsMap;

    // Set pin 4 mode (special function 0: general purpose clock 0)
    ACCESS(peripherals, GPIO_BASE) = (ACCESS(peripherals, GPIO_BASE)
                                       & 0xFFFF8FFF) | (0x01 << 14);

    // Configure general purpose clock 0
    ACCESS(peripherals, CLK0_BASE) = (0x5A << 24) // clock manager password
                                      | (0x01 << 9) // 1-stage MASH
                                      | (0x01 << 4) // enable clock generator
                                      | 0x06; // source: PLLD per

    /* Compute base clock divisor
       - PLLD operates at 500 MHz according to http://goo.gl/p96dJ0
    */
    clockDivisor = (unsigned int)((500 << 12) / frequency + 0.5);
}

void Transmitter::transmit(std::vector<float> *samples, unsigned int sampleRate)
{
    unsigned int temp, offset = 0, length = samples->size();

    // Access system timer free-running counter
    unsigned long long current, start = ACCESS64(peripherals, SYST_BASE + 0x4);

    while ((temp = offset) < length) {

        // Set clock divisor (effectively, modulate sample)
        ACCESS(peripherals, CLK0D_BASE) = (0x5A << 24) // clock manager password
                     | (clockDivisor - (int)(round((*samples)[offset] * 16.0)));

        // Wait for sample to be transmitted (microsecond resolution)
        while (temp >= offset) {
            usleep(1);
            current = ACCESS64(peripherals, SYST_BASE + 0x4);
            offset = (unsigned int)((current - start) * sampleRate / 1000000);
        }
    }
}

Transmitter::~Transmitter()
{
    ACCESS(peripherals, CLK0_BASE) = (0x5A << 24); // reset GPC0
    munmap((void *)peripherals, 0x002FFFFF);
}
