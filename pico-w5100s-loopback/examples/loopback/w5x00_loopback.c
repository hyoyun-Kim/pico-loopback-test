/**
 * Copyright (c) 2021 WIZnet Co.,Ltd
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
  * ----------------------------------------------------------------------------------------------------
  * Includes
  * ----------------------------------------------------------------------------------------------------
  */
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"

#include "wizchip_conf.h"
#include "w5100s.h"

#include "loopback.h"

/**
  * ----------------------------------------------------------------------------------------------------
  * Macros
  * ----------------------------------------------------------------------------------------------------
  */

/* SPI */
#define SPI_PORT spi0

#define PIN_SCK 18
#define PIN_MOSI 19
#define PIN_MISO 16
#define PIN_CS 17
#define PIN_RST 20

/* Buffer */
#define ETHERNET_BUF_MAX_SIZE (1024 * 2)

/* Socket */
#define SOCKET_LOOPBACK 0

/* Port */
#define PORT_LOOPBACK 5000

/* Clock */
#define PLL_SYS_KHZ (50 * 1000)

/* Use SPI DMA */
#define USE_SPI_DMA // if you want to use SPI DMA, uncomment.

/**
  * ----------------------------------------------------------------------------------------------------
  * Variables
  * ----------------------------------------------------------------------------------------------------
  */
/* Network */
static wiz_NetInfo g_net_info =
    {
        .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56}, // MAC address
        .ip = {192, 168, 1, 15},                     // IP address
        .sn = {255, 255, 255, 0},                    // Subnet Mask
        .gw = {192, 168, 1, 1},                      // Gateway
        .dns = {8, 8, 8, 8},                         // DNS server
        .dhcp = NETINFO_STATIC                       // DHCP
};

/* WIZ5100S PHY configuration for link speed 10MHz */
wiz_PhyConf gPhyConf = {.by = PHY_CONFBY_SW,
                        .mode = PHY_MODE_MANUAL,
                        .speed = PHY_SPEED_10,
                        .duplex = PHY_DUPLEX_FULL};

/* Loopback */
static uint8_t g_loopback_buf[ETHERNET_BUF_MAX_SIZE] = {
    0,
};
static uint8_t g_loopback_socket = SOCKET_LOOPBACK;
static uint16_t g_loopback_port = PORT_LOOPBACK;

/**
  * ----------------------------------------------------------------------------------------------------
  * Functions
  * ----------------------------------------------------------------------------------------------------
  */
/* W5x00 */
static inline void wizchip_select(void);
static inline void wizchip_deselect(void);
static void wizchip_reset(void);
static uint8_t wizchip_read(void);
static void wizchip_write(uint8_t tx_data);

#ifdef USE_SPI_DMA
static void wizchip_read_burst(uint8_t *pBuf, uint16_t len);
static void wizchip_write_burst(uint8_t *pBuf, uint16_t len);
#endif

static void wizchip_initialize(void);
static void wizchip_check(void);

/* Network */
static void network_initialize(void);
static void print_network_information(void);

#ifdef USE_SPI_DMA
uint dma_tx;
uint dma_rx;
dma_channel_config dma_channel_config_tx;
dma_channel_config dma_channel_config_rx;
#endif

/**
  * ----------------------------------------------------------------------------------------------------
  * Main
  * ----------------------------------------------------------------------------------------------------
  */
int main()
{
    /* Initialize */
    int32_t retval = 0;
    uint32_t spi_badurate = 0;    

    stdio_init_all();

    // set main clock to 50MHz
    set_sys_clock_khz(PLL_SYS_KHZ, true);

    // clock_configuration
    clock_configure(
        clk_peri,
        0,                                                // No glitchless mux
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, // System PLL on AUX mux
        PLL_SYS_KHZ * 1000,                               // Input frequency
        PLL_SYS_KHZ * 1000                                // Output (must be same as no divider)
    );

    // this example will use SPI0 at 25MHz
    spi_badurate = spi_init(SPI_PORT, 50000 * 1000);

    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);

    // make the SPI pins available to picotool
    bi_decl(bi_3pins_with_func(PIN_MISO, PIN_MOSI, PIN_SCK, GPIO_FUNC_SPI));

    // chip select is active-low, so we'll initialise it to a driven-high state
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    // make the SPI pins available to picotool
    bi_decl(bi_1pin_with_name(PIN_CS, "W5x00 CHIP SELECT"));

#ifdef USE_SPI_DMA
    dma_tx = dma_claim_unused_channel(true);
    dma_rx = dma_claim_unused_channel(true);

    dma_channel_config_tx = dma_channel_get_default_config(dma_tx);
    channel_config_set_transfer_data_size(&dma_channel_config_tx, DMA_SIZE_8);
    channel_config_set_dreq(&dma_channel_config_tx, DREQ_SPI0_TX);

    // We set the inbound DMA to transfer from the SPI receive FIFO to a memory buffer paced by the SPI RX FIFO DREQ
    // We coinfigure the read address to remain unchanged for each element, but the write
    // address to increment (so data is written throughout the buffer)
    dma_channel_config_rx = dma_channel_get_default_config(dma_rx);
    channel_config_set_transfer_data_size(&dma_channel_config_rx, DMA_SIZE_8);
    channel_config_set_dreq(&dma_channel_config_rx, DREQ_SPI0_RX);
    channel_config_set_read_increment(&dma_channel_config_rx, false);
    channel_config_set_write_increment(&dma_channel_config_rx, true);
#endif

    
    wizchip_reset();
    wizchip_initialize();
    wizchip_check();
    
    // set the w5100s chip to link speed 10MHz
    ctlwizchip(CW_SET_PHYCONF, &gPhyConf);
    ctlwizchip(CW_RESET_PHY, 0);
    
    network_initialize();
    sleep_ms(3000); 

    // get network information
    print_network_information();
    
#ifdef USE_SPI_DMA
    printf("This is DMA version\n");
#else
    printf("This is without DMA version\n");
#endif

    // get spi baudrate
    printf("spi_init return = %dHz\r\n", spi_badurate);

    /* Infinite loop */
    while (1)
    {
        if ((retval = loopback_tcps(g_loopback_socket, g_loopback_buf, g_loopback_port)) < 0)
        {
            printf(" Loopback error : %d\n", retval);
        }
    }
}

/**
  * ----------------------------------------------------------------------------------------------------
  * Functions
  * ----------------------------------------------------------------------------------------------------
  */
/* W5x00 */
static inline void wizchip_select(void)
{
    gpio_put(PIN_CS, 0);
}

static inline void wizchip_deselect(void)
{
    gpio_put(PIN_CS, 1);
}

static void wizchip_reset(void)
{
    gpio_set_dir(PIN_RST, GPIO_OUT);

    gpio_put(PIN_RST, 0);
    sleep_ms(100);

    gpio_put(PIN_RST, 1);
    sleep_ms(100);

    bi_decl(bi_1pin_with_name(PIN_RST, "W5x00 RESET"));
}

static uint8_t wizchip_read(void)
{
    uint8_t rx_data = 0;
    uint8_t tx_data = 0xFF;

    spi_read_blocking(SPI_PORT, tx_data, &rx_data, 1);

    return rx_data;
}

static void wizchip_write(uint8_t tx_data)
{
    spi_write_blocking(SPI_PORT, &tx_data, 1);
}

#ifdef USE_SPI_DMA
static void wizchip_read_burst(uint8_t *pBuf, uint16_t len)
{
    uint8_t dummy_data = 0xFF;

    channel_config_set_read_increment(&dma_channel_config_tx, false);
    channel_config_set_write_increment(&dma_channel_config_tx, false);
    dma_channel_configure(dma_tx, &dma_channel_config_tx,
                          &spi_get_hw(SPI_PORT)->dr, // write address
                          &dummy_data,               // read address
                          len,                       // element count (each element is of size transfer_data_size)
                          false);                    // don't start yet

    channel_config_set_read_increment(&dma_channel_config_rx, false);
    channel_config_set_write_increment(&dma_channel_config_rx, true);
    dma_channel_configure(dma_rx, &dma_channel_config_rx,
                          pBuf,                      // write address
                          &spi_get_hw(SPI_PORT)->dr, // read address
                          len,                       // element count (each element is of size transfer_data_size)
                          false);                    // don't start yet

    dma_start_channel_mask((1u << dma_tx) | (1u << dma_rx));
    dma_channel_wait_for_finish_blocking(dma_rx);
}

static void wizchip_write_burst(uint8_t *pBuf, uint16_t len)
{
    uint8_t dummy_data;
    //uint8_t dummy_arry[1024];

    channel_config_set_read_increment(&dma_channel_config_tx, true);
    channel_config_set_write_increment(&dma_channel_config_tx, false);
    dma_channel_configure(dma_tx, &dma_channel_config_tx,
                          &spi_get_hw(SPI_PORT)->dr, // write address
                          pBuf,                      // read address
                          len,                       // element count (each element is of size transfer_data_size)
                          false);                    // don't start yet

    channel_config_set_read_increment(&dma_channel_config_rx, false);
    channel_config_set_write_increment(&dma_channel_config_rx, false);
    dma_channel_configure(dma_rx, &dma_channel_config_rx,
                          &dummy_data,               // write address
                          &spi_get_hw(SPI_PORT)->dr, // read address
                          len,                       // element count (each element is of size transfer_data_size)
                          false);                    // don't start yet

    dma_start_channel_mask((1u << dma_tx) | (1u << dma_rx));
    dma_channel_wait_for_finish_blocking(dma_rx);
}
#endif

static void wizchip_initialize(void)
{
    /* Deselect the FLASH : chip select high */
    wizchip_deselect();

    /* CS function register */
    reg_wizchip_cs_cbfunc(wizchip_select, wizchip_deselect);

    /* SPI function register */
    reg_wizchip_spi_cbfunc(wizchip_read, wizchip_write);
#ifdef USE_SPI_DMA
    reg_wizchip_spiburst_cbfunc(wizchip_read_burst, wizchip_write_burst);
#endif
    /* W5x00 initialize */
    uint8_t temp;
    uint8_t memsize[2][8] = {{2, 2, 2, 2, 2, 2, 2, 2}, {2, 2, 2, 2, 2, 2, 2, 2}};

    if (ctlwizchip(CW_INIT_WIZCHIP, (void *)memsize) == -1)
    {
        printf(" W5x00 initialized fail\n");

        return;
    }

    /* PHY link status check */
    do
    {
        if (ctlwizchip(CW_GET_PHYLINK, (void *)&temp) == -1)
        {
            printf(" Unknown PHY link status\n");

            return;
        }
    } while (temp == PHY_LINK_OFF);
}

static void wizchip_check(void)
{
    /* Read version register */
    if (getVER() != 0x51) // W5100S
    {
        printf(" ACCESS ERR : VERSIONR != 0x51, read value = 0x%02x\n", getVER());

        while (1)
            ;
    }
}

/* Network */
static void network_initialize(void)
{
    ctlnetwork(CN_SET_NETINFO, (void *)&g_net_info);
}

static void print_network_information(void)
{
    uint8_t tmp_str[8] = {
        0,
    };

    ctlnetwork(CN_GET_NETINFO, (void *)&g_net_info);
    ctlwizchip(CW_GET_ID, (void *)tmp_str);

    if (g_net_info.dhcp == NETINFO_DHCP)
    {
        printf("====================================================================================================\n");
        printf(" %s network configuration : DHCP\n\n", (char *)tmp_str);
    }
    else
    {
        printf("====================================================================================================\n");
        printf(" %s network configuration : static\n\n", (char *)tmp_str);
    }

    printf(" MAC         : %02X:%02X:%02X:%02X:%02X:%02X\n", g_net_info.mac[0], g_net_info.mac[1], g_net_info.mac[2], g_net_info.mac[3], g_net_info.mac[4], g_net_info.mac[5]);
    printf(" IP          : %d.%d.%d.%d\n", g_net_info.ip[0], g_net_info.ip[1], g_net_info.ip[2], g_net_info.ip[3]);
    printf(" Subnet Mask : %d.%d.%d.%d\n", g_net_info.sn[0], g_net_info.sn[1], g_net_info.sn[2], g_net_info.sn[3]);
    printf(" Gateway     : %d.%d.%d.%d\n", g_net_info.gw[0], g_net_info.gw[1], g_net_info.gw[2], g_net_info.gw[3]);
    printf(" DNS         : %d.%d.%d.%d\n", g_net_info.dns[0], g_net_info.dns[1], g_net_info.dns[2], g_net_info.dns[3]);
    printf("====================================================================================================\n\n");
}
