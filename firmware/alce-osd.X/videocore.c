/*
    AlceOSD - Graphical OSD
    Copyright (C) 2015  Luis Alves

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "alce-osd.h"


/* enable ram output */
#define OE_RAM      LATBbits.LATB8
#define OE_RAM_DIR  TRISBbits.TRISB8
#define OE_RAM2     LATAbits.LATA9
#define OE_RAM_DIR2 TRISAbits.TRISA9


/* sram commands */
#define SRAM_READ   0x03
#define SRAM_WRITE  0x02
#define SRAM_DIO    0x3b
#define SRAM_QIO    0x38
#define SRAM_RSTIO  0xFF
#define SRAM_RMODE  0x05
#define SRAM_WMODE  0x01

#define SRAM_SIZE (0x20000)

/* chip select */
#define CS_DIR TRISCbits.TRISC9
#define CS_LOW LATCbits.LATC9 = 0
#define CS_HIGH LATCbits.LATC9 = 1
/* clock */
#define CLK_DIR TRISCbits.TRISC8
#define CLK_LOW LATCbits.LATC8 = 0
#define CLK_HIGH LATCbits.LATC8 = 1
/* data direction */
#define SRAM_SPI TRISCbits.TRISC0 = 0; TRISCbits.TRISC1 = 1
#define SRAM_OUT TRISC &= 0xfffc
#define SRAM_IN TRISC |= 0x000f
#define SRAM_OUTQ TRISC &= 0xfff0
#define SPI_O LATCbits.LATC0
#define SPI_I PORTCbits.RC1
/* spi2 clock pin */
#define SCK2_O 0x09


#define LINE_TMR (278*12)-2

#define INT_X_OFFSET    (125)
#define CNT_INT_MODE    (10 * 1000)

#define CTRL_SYNCGEN        0x01
#define CTRL_COMPSYNC       0x02
#define CTRL_PWMBRIGHT      0x04
#define CTRL_DACBRIGHT      0x08

//#define DEBUG_DAC

extern struct alceosd_config config;
extern unsigned char hw_rev;

void video_apply_config_cbk(void);

const struct param_def params_video[] = {
    PARAM("VIDE0_STD", MAV_PARAM_TYPE_UINT8, &config.video[0].mode, NULL),
    PARAM("VIDE0_XSIZE", MAV_PARAM_TYPE_UINT8, &config.video[0].x_size_id, video_apply_config_cbk),
    PARAM("VIDE0_YSIZE", MAV_PARAM_TYPE_UINT16, &config.video[0].y_size, NULL),
    PARAM("VIDE0_XOFFSET", MAV_PARAM_TYPE_UINT16, &config.video[0].x_offset, NULL),
    PARAM("VIDE0_YOFFSET", MAV_PARAM_TYPE_UINT16, &config.video[0].y_offset, NULL),

    PARAM("VIDE1_STD", MAV_PARAM_TYPE_UINT8, &config.video[1].mode, NULL),
    PARAM("VIDE1_XSIZE", MAV_PARAM_TYPE_UINT8, &config.video[1].x_size_id, video_apply_config_cbk),
    PARAM("VIDE1_YSIZE", MAV_PARAM_TYPE_UINT16, &config.video[1].y_size, NULL),
    PARAM("VIDE1_XOFFSET", MAV_PARAM_TYPE_UINT16, &config.video[1].x_offset, NULL),
    PARAM("VIDE1_YOFFSET", MAV_PARAM_TYPE_UINT16, &config.video[1].y_offset, NULL),

    PARAM_END,
};
const struct param_def params_video0v1[] = {
    PARAM("VIDE0_BRIGHT", MAV_PARAM_TYPE_UINT16, &config.video[0].brightness, video_apply_config_cbk),
    PARAM("VIDE1_BRIGHT", MAV_PARAM_TYPE_UINT16, &config.video[1].brightness, video_apply_config_cbk),
    PARAM_END,
};
const struct param_def params_video0v3[] = {
    PARAM("VIDE0_WHITE", MAV_PARAM_TYPE_UINT8, &config.video[0].white_lvl, video_apply_config_cbk),
    PARAM("VIDE0_GRAY", MAV_PARAM_TYPE_UINT8, &config.video[0].gray_lvl, video_apply_config_cbk),
    PARAM("VIDE0_BLACK", MAV_PARAM_TYPE_UINT8, &config.video[0].black_lvl, video_apply_config_cbk),

    PARAM("VIDE1_WHITE", MAV_PARAM_TYPE_UINT8, &config.video[1].white_lvl, video_apply_config_cbk),
    PARAM("VIDE1_GRAY", MAV_PARAM_TYPE_UINT8, &config.video[1].gray_lvl, video_apply_config_cbk),
    PARAM("VIDE1_BLACK", MAV_PARAM_TYPE_UINT8, &config.video[1].black_lvl, video_apply_config_cbk),
    PARAM_END,
};


static volatile unsigned int int_sync_cnt = 0;
volatile unsigned char sram_busy = 0;
volatile unsigned int line, last_line_cnt = 0;
volatile unsigned char odd = 0;
static struct canvas *rendering_canvas = NULL;

static unsigned char videocore_ctrl = 0;

static struct video_config *cfg = &config.video[0];



const struct osd_xsize_tbl video_xsizes[] = {
    { .xsize = 420, .clk_ps = 0 } ,
    { .xsize = 480, .clk_ps = 1 } ,
    { .xsize = 560, .clk_ps = 2 } ,
    { .xsize = 672, .clk_ps = 3 } ,
};


#define MAX_CANVAS_PIPE_MASK (0x1f)

static struct canvas_pipe_s {
    struct canvas *ca[MAX_CANVAS_PIPE_MASK+1];
    unsigned char prd, pwr;
    unsigned char peak;
} canvas_pipe = {
    .pwr = 0,
    .prd = 0,
};


#define SCRATCHPAD_SIZE 0x4000
__eds__ unsigned char scratchpad1[SCRATCHPAD_SIZE]  __attribute__ ((eds, noload, address(0x4000)));
__eds__ unsigned char scratchpad2[SCRATCHPAD_SIZE]  __attribute__ ((eds, noload, address(0x8000)));

struct scratchpad_s {
    __eds__ unsigned char *mem;
    unsigned int alloc_size;
};

struct scratchpad_s scratchpad[2] = {
    {   .mem = scratchpad1,
        .alloc_size = 0,     },
    {   .mem = scratchpad2,
        .alloc_size = 0,     },
};


unsigned char sram_byte_spi(unsigned char b);
extern void sram_byteo_sqi(unsigned char b);
extern unsigned char sram_bytei_sqi(void);
extern __eds__ unsigned char* copy_line(__eds__ unsigned char *buf, unsigned int count);
extern void clear_canvas(__eds__ unsigned char *buf, unsigned int count, unsigned char v);



static void sram_exit_sqi(void)
{
    CS_LOW;
    SRAM_OUTQ;
    LATC = (LATC & 0xfff0) | 0x000f;
    CLK_HIGH;CLK_LOW;
    CLK_HIGH;CLK_LOW;
    CS_HIGH;
    SRAM_SPI;
}

static void sram_exit_sdi(void)
{
    CS_LOW;
    SRAM_OUTQ;
    LATC = (LATC & 0xfff0) | 0x000f;
    CLK_HIGH;CLK_LOW;
    CLK_HIGH;CLK_LOW;
    CLK_HIGH;CLK_LOW;
    CLK_HIGH;CLK_LOW;
    CS_HIGH;
    SRAM_SPI;
}

unsigned char sram_byte_spi(unsigned char b)
{
    register unsigned char i;
    unsigned char out = 0;

    for (i = 0; i < 8; i++) {
        out <<= 1;
        CLK_LOW;
        if (b & 0x80)
            SPI_O = 1;
        else
            SPI_O = 0;
        b <<= 1;
        CLK_HIGH;
        if (SPI_I)
            out |= 1;
    }
    /* clk = 0 */
    CLK_LOW;
    return out;
}

void sram_byteo_sdi(unsigned char b)
{
    unsigned int _LATC = LATC & 0xfefc; /* data clear and clk low*/

    LATC = _LATC | ((b >> 6));
    CLK_HIGH;
    LATC = _LATC | ((b >> 4) & 3);
    CLK_HIGH;
    LATC = _LATC | ((b >> 2) & 3);
    CLK_HIGH;
    LATC = _LATC | (b & 3);
    CLK_HIGH;
    CLK_LOW;
}

void clear_sram(void)
{
    register unsigned long i;

    CS_LOW;
    sram_byteo_sqi(SRAM_WRITE);
    sram_byteo_sqi(0);
    sram_byteo_sqi(0);
    sram_byteo_sqi(0);
    for (i = 0; i < SRAM_SIZE; i++) {
        sram_byteo_sqi(0x00);
    }
    CS_HIGH;
}

static unsigned char test_sram_0(unsigned char b)
{
    register unsigned long i;
    register unsigned char r;

    CS_LOW;
    sram_byteo_sqi(SRAM_WRITE);
    sram_byteo_sqi(0);
    sram_byteo_sqi(0);
    sram_byteo_sqi(0);
    for (i = 0; i < SRAM_SIZE; i++) {
        sram_byteo_sqi(b);
    }
    CS_HIGH;
    
    CS_LOW;
    sram_byteo_sqi(SRAM_READ);
    sram_byteo_sqi(0);
    sram_byteo_sqi(0);
    sram_byteo_sqi(0);
    SRAM_IN;
    r = sram_bytei_sqi();
    for (i = 0; i < SRAM_SIZE; i++) {
        r = sram_bytei_sqi();
        if (r != b) {
            CS_HIGH;
            SRAM_OUTQ;
            printf("test_ram: expected = 0x%02x read = 0x%02x\n", b, r);
            return 1;
        }
    }
    CS_HIGH;
    SRAM_OUTQ;
    return 0;
}

static void test_sram(void)
{
    unsigned char test[4] = {0x00, 0x5a, 0xa5, 0xff};
    unsigned char i, j;
    printf("Testing SRAM\n");
    
    for (i = 0; i < 4; i++) {
        j = test[i];
        if (test_sram_0(j))
            printf("0x%02x (fail)\n", j);
        else
            printf("0x%02x (pass)\n", j);
    }
}

static void video_init_sram(void)
{
    /* cs as output, set high */
    CS_DIR = 0;
    CS_HIGH;
    /* clock as output, set low */
    CLK_DIR = 0;
    CLK_LOW;
    if (hw_rev == 0x03)
        _RP56R = 0;
    else
        _RP54R = 0;

    /* force a spi mode from sdi */
    sram_exit_sdi();
    /* force a spi mode from sqi */
    sram_exit_sqi();

    /* set mode register (sequential rw mode) */
    CS_LOW;
    sram_byte_spi(SRAM_WMODE);
    sram_byte_spi(0x40);
    CS_HIGH;

    /* IOs are now in spi mode, set SQI */
    CS_LOW;
    sram_byte_spi(SRAM_QIO);
    CS_HIGH;
    SRAM_OUTQ;

    /* test SRAM */
    //test_sram();

    /* set ram to zeros */
    clear_sram();

#if 0
    unsigned long i, k;

    CS_LOW;
    sram_byteo_sqi(SRAM_WRITE);
    sram_byteo_sqi(0);
    sram_byteo_sqi(0);
    sram_byteo_sqi(0);
    //for (i = 0; i < OSD_YSIZE; i++) {
        for (k = 0; k < OSD_XSIZE/8; k++) {
            //sram_byteo_sqi(0x41);
            sram_byteo_sqi(0x05);
            sram_byteo_sqi(0xaf);
       }
   // }
    CS_HIGH;
#endif
}

#define VIDEO_DAC_BUF_SIZE  (3*2*4)
static void video_read_dac(unsigned char *dac_status)
{
    //unsigned char dac_status[VIDEO_DAC_BUF_SIZE];
    unsigned char i;
    
    I2C1CONbits.SEN = 1;
    while (I2C1CONbits.SEN == 1);
    
    I2C1TRN = 0xc1;
    while (I2C1STATbits.TRSTAT == 1);

    for (i = 0; i < VIDEO_DAC_BUF_SIZE; i++) {
        I2C1CONbits.RCEN = 1;
        while (I2C1CONbits.RCEN == 1);
        
        dac_status[i] = I2C1RCV;
        
        I2C1CONbits.ACKEN = 1;
        while (I2C1CONbits.ACKEN == 1);
    }

    I2C1CONbits.PEN = 1;
    while (I2C1CONbits.PEN == 1);
    
    /*for (i = 0; i < VIDEO_DAC_BUF_SIZE; i++) {
        printf("0x%02x ", dac_status[i]);
        if (((i+1) % 3) == 0)
            printf("\n");
    }*/
    
    
}

static void video_init_dac(void)
{
    /* init (a)i2c1*/
    _ODCB8 = 1;
    _ODCB9 = 1;
    
    I2C1CON = 0x8000;
    
    I2C1ADD = 0;
    I2C1MSK = 0;
    
    I2C1BRG = 0x1ff;
}


static void video_update_dac(struct video_config *cfg)
{
    unsigned int dac_values[4];
    unsigned char i;
    
    dac_values[0] = cfg->white_lvl << 4;
    dac_values[1] = cfg->gray_lvl << 4;
    dac_values[2] = cfg->black_lvl << 4;
    dac_values[3] = 0;

    I2C1CONbits.SEN = 1;
    while (I2C1CONbits.SEN == 1);
    
    I2C1TRN = 0xc0;
    while (I2C1STATbits.TRSTAT == 1);

    for (i = 0; i < 4; i++) {
        I2C1TRN = (dac_values[i] >> 8) & 0x0f;
        while (I2C1STATbits.TRSTAT == 1);
    
        I2C1TRN = dac_values[i] & 0xff;
        while (I2C1STATbits.TRSTAT == 1);

    }
    I2C1CONbits.PEN = 1;
    while (I2C1CONbits.PEN == 1);
}


static void video_init_hw(void)
{
    switch (hw_rev) {
        case 0x01:
        default:
            videocore_ctrl |= CTRL_PWMBRIGHT;
            break;
        case 0x02:
            videocore_ctrl |= CTRL_PWMBRIGHT;
            videocore_ctrl |= CTRL_SYNCGEN;
            break;
        case 0x03:
            videocore_ctrl |= CTRL_SYNCGEN;
            videocore_ctrl |= CTRL_DACBRIGHT;
            videocore_ctrl |= CTRL_COMPSYNC;
            break;
    }


    SPI2CON1bits.CKP = 0; /* idle low */
    SPI2CON1bits.CKE = 1;
    SPI2CON1bits.MSTEN = 1;
    SPI2CON1bits.DISSDO = 1;
    SPI2CON1bits.DISSCK = 0;
    SPI2CON1bits.PPRE = 3;
    SPI2CON1bits.SPRE = video_xsizes[cfg->x_size_id].clk_ps;
    SPI2CON1bits.MODE16 = 1;
    SPI2CON2bits.FRMEN = 1;
    /* pins as ports */
    //_RP12R = SCK2_O;
    //_RP0R = SDO2_O;
    SPI2STATbits.SPIROV = 0;
    SPI2STATbits.SPIEN = 0;

    if (hw_rev <= 0x02) {
        OE_RAM_DIR = 0;
        OE_RAM = 1;
    } else {
        OE_RAM_DIR2 = 0;
        OE_RAM2 = 1;
        /* mux as input */
        _TRISA2 = 1;
        _TRISA3 = 1;
        _LATA2 = 0;
        _LATA3 = 0;
        /* pull downs */
        _CNPUA2 = 0;
        _CNPDA2 = 1;
        _CNPUA3 = 0;
        _CNPDA3 = 1;
    }
    
    /* generic line timer */
    T2CONbits.T32 = 0;
    T2CONbits.TCKPS = 0;
    T2CONbits.TCS = 0;
    T2CONbits.TGATE = 0;
    T2CONbits.TON = 0;
    _T2IP = 6;
    IFS0bits.T2IF = 0;
    IEC0bits.T2IE = 1;


    if ((videocore_ctrl & CTRL_COMPSYNC) == 0) {
        /* csync, vsync, frame */
        TRISBbits.TRISB13 = 1;
        TRISBbits.TRISB14 = 1;
        TRISBbits.TRISB15 = 1;

        /* CSYNC - INT2 */
        RPINR1bits.INT2R = 45;
        /* falling edge */
        INTCON2bits.INT2EP = 1;
        /* priority */
        _INT2IP = 4;

        /* VSYNC - INT1 */
        RPINR0bits.INT1R = 46;
        /* falling edge */
        INTCON2bits.INT1EP = 1;
        /* priority */
        _INT1IP = 3;
        /* enable int1 */
        _INT1IE = 1;
    }


    if (videocore_ctrl & CTRL_PWMBRIGHT) {
        /* brightness */
        /* OC1 pin */
        TRISBbits.TRISB7 = 0;
        _RP39R = 0x10;

        T3CONbits.TCKPS = 0;
        T3CONbits.TCS = 0;
        T3CONbits.TGATE = 0;
        TMR3 = 0x00;
        PR3 = 0x7fff;
        T3CONbits.TON = 1;

        OC1CON1bits.OCM = 0;
        OC1CON1bits.OCTSEL = 1;
        OC1CON2bits.SYNCSEL = 0x1f;
        OC1R = 0x100;
        OC1RS = 0x100 + cfg->brightness;
        OC1CON1bits.OCM = 0b110;
    }

    
    if (videocore_ctrl & CTRL_SYNCGEN) {
        /* sync pin */
        if (hw_rev <= 0x02) {
            _TRISA9 = 0;
            _LATA9 = 1;
        } else {
            _TRISA4 = 0;
            _LATA4 = 1;
        }

        /* timer */
        T4CON = 0x8010;
        _T4IP = 3;
        /* period = 1 / (70000000 / 8) * 56 = 6.4us */
        PR4 = 56;
    }

    if (videocore_ctrl & CTRL_COMPSYNC) {
        /* analog input */
        TRISBbits.TRISB0 = 1;
        ANSELBbits.ANSB0 = 1;

        /* vref */
#if defined (__dsPIC33EP512GM604__)
        CVR1CONbits.CVR1OE = 0;
//        CVR1CONbits.CVR2OE = 0;
        CVR1CONbits.VREFSEL = 0;
        CVR1CONbits.CVRR = 1;
        CVR1CONbits.CVRSS = 0;
        CVR1CONbits.CVR = 4;
        CVR1CONbits.CVREN = 1;
#else
        CVRCONbits.CVR1OE = 0;
        CVRCONbits.CVR2OE = 0;
        CVRCONbits.VREFSEL = 0;
        CVRCONbits.CVRR = 1;
        CVRCONbits.CVRSS = 0;
        CVRCONbits.CVR = 3;
        CVRCONbits.CVREN = 1;
#endif

        _RP54R = 0x19;
        _IC1R = 54;
        /* input capture */
        IC1CON1bits.ICM = 0b000;
        IC1CON1bits.ICTSEL = 7;
        IC1CON2bits.ICTRIG = 0;
        IC1CON2bits.SYNCSEL = 0;
        IC1CON1bits.ICI = 0;
        IPC0bits.IC1IP = 4;
        IFS0bits.IC1IF = 0;
        IEC0bits.IC1IE = 1;
        IC1CON1bits.ICM = 0b001;

        /* comp */
        CM2CONbits.COE = 1;
        CM2CONbits.CPOL = 1;
        CM2CONbits.OPMODE = 0;
        CM2CONbits.COUT = 0;
        CM2CONbits.EVPOL = 0b00;
        CM2CONbits.CREF = 1;
        CM2CONbits.CCH = 0b00;
        CM2FLTRbits.CFLTREN = 0;
        CM2CONbits.CON = 1;

        T5CON = 0x0010;
        /* period = 1 / (70000000 / 8) * PR5 */
        PR5 = 500;
        _T5IP = 7;
        _T5IF = 0;
        _T5IE = 1;
    }
    
    if (videocore_ctrl & CTRL_DACBRIGHT) {
        video_init_dac();
#ifdef DEBUG_DAC
        video_update_dac(&config.video);
        video_read_dac();
#endif
    }
}

void video_apply_config(unsigned char profile)
{
    if (profile < CONFIG_MAX_VIDEO)
        cfg = &config.video[profile];

    /* brightness */
    if (hw_rev < 0x03) {
        OC1RS = 0x100 + cfg->brightness;
    } else {
        video_update_dac(cfg);
    }

    /* pixel clock */
    INTCON2bits.GIE = 0;
    SPI2STATbits.SPIEN = 0;
    SPI2CON1bits.SPRE = video_xsizes[cfg->x_size_id].clk_ps;
    INTCON2bits.GIE = 1;

    if (cfg->mode & VIDEO_MODE_SYNC_MASK) {
        _T4IF = 0;
        _T4IE = 1;
    } else {
        _T4IE = 0;
    }
}

void video_apply_config_cbk(void)
{
    if (cfg->x_size_id >= VIDEO_XSIZE_END)
        cfg->x_size_id = VIDEO_XSIZE_END - 1;
    video_apply_config(VIDEO_ACTIVE_CONFIG);
}

void video_get_size(unsigned int *xsize, unsigned int *ysize)
{
    *xsize = video_xsizes[cfg->x_size_id].xsize;
    *ysize = cfg->y_size;
    if (cfg->mode & VIDEO_MODE_SCAN_MASK)
        *ysize *= 2;
}

void video_pause(void)
{
    while (sram_busy);

    if (videocore_ctrl & CTRL_SYNCGEN) {
        _T4IE = 0;
    }

    if (videocore_ctrl & CTRL_COMPSYNC) {
        _IC1IE = 0;
    } else {
        _INT2IE = 0;
        _INT1IE = 0;
    }
}

void video_resume(void)
{
    if (videocore_ctrl & CTRL_COMPSYNC) {
        _IC1IF = 0;
        _IC1IE = 1;
    } else {
        _INT1IF = 0;
        _INT1IE = 1;
    }

    if ((videocore_ctrl & CTRL_SYNCGEN) && (cfg->mode & VIDEO_MODE_SYNC_MASK)) {
        _T4IF = 0;
        _T4IE = 1;
    }
}

void free_mem(void)
{
    scratchpad[0].alloc_size = 0;
    scratchpad[1].alloc_size = 0;

    canvas_pipe.prd = canvas_pipe.pwr = 0;
    rendering_canvas = NULL;
}



int alloc_canvas(struct canvas *c, void *widget_cfg)
{
    unsigned int osdxsize, osdysize, i;
    struct widget_config *wcfg = widget_cfg;
    video_get_size(&osdxsize, &osdysize);

    c->width = (c->width & 0xfffc);
    c->rwidth = c->width >> 2;
    c->size = c->rwidth * c->height;

    for (i = 0; i < 2; i++) {
        if ((scratchpad[i].alloc_size + c->size) < SCRATCHPAD_SIZE)
            break;
    }
    if (i == 2) {
        c->lock = 1;
        return -1;
    }

    switch (wcfg->props.vjust) {
        case VJUST_TOP:
        default:
            c->y = wcfg->y;
            break;
        case VJUST_BOT:
            c->y = osdysize - c->height + wcfg->y;
            break;
        case VJUST_CENTER:
            c->y = (osdysize - c->height)/2 + wcfg->y;
            break;
    }

    switch (wcfg->props.hjust) {
        case HJUST_LEFT:
        default:
            c->x = wcfg->x;
            break;
        case HJUST_RIGHT:
            c->x = osdxsize - c->width + wcfg->x;
            break;
        case HJUST_CENTER:
            c->x = (osdxsize - c->width)/2 + wcfg->x;
            break;
    }

    c->buf = &scratchpad[i].mem[scratchpad[i].alloc_size];
    scratchpad[i].alloc_size += c->size;

    c->lock = 0;
    return 0;
}


int init_canvas(struct canvas *ca, unsigned char b)
{
    if (ca->lock)
        return -1;
    clear_canvas(ca->buf, ca->size, b);
    return 0;
}


void schedule_canvas(struct canvas *ca)
{
    canvas_pipe.ca[canvas_pipe.pwr++] = ca;
    canvas_pipe.pwr &= MAX_CANVAS_PIPE_MASK;
    canvas_pipe.peak = MAX(canvas_pipe.peak, canvas_pipe.pwr - canvas_pipe.prd);
    ca->lock = 1;
}


static void render_process(void)
{
    static unsigned int y1, y;
    __eds__ static unsigned char *b;
    static union sram_addr addr;
    static unsigned int xsize;

    unsigned int x;

    for (;;) {
        if (rendering_canvas == NULL) {
            if (canvas_pipe.prd == canvas_pipe.pwr)
                return;

            rendering_canvas = canvas_pipe.ca[canvas_pipe.prd];

            y = rendering_canvas->y;
            y1 = rendering_canvas->y + rendering_canvas->height;
            x = rendering_canvas->x >> 2;
            b = rendering_canvas->buf;

            xsize = (video_xsizes[cfg->x_size_id].xsize) >> 2;
            addr.l = x + ((unsigned long) xsize *  y);
        } else {
            /* render */
            for (;;) {
                if (sram_busy)
                    return;

                CS_LOW;
                sram_byteo_sqi(SRAM_WRITE);
                sram_byteo_sqi(addr.b2);
                sram_byteo_sqi(addr.b1);
                sram_byteo_sqi(addr.b0);
                b = copy_line(b, rendering_canvas->rwidth);
                CS_HIGH;

                if (++y == y1)
                    break;

                addr.l += xsize;
            }
            rendering_canvas->lock = 0;
            canvas_pipe.prd++;
            canvas_pipe.prd &= MAX_CANVAS_PIPE_MASK;
            rendering_canvas = NULL;
        }
    }
}


/* blocking render */
void render_canvas(struct canvas *ca)
{
    unsigned int x, h;
    __eds__ unsigned char *b;
    union sram_addr addr;
    unsigned int osdxsize, osdysize;
    video_get_size(&osdxsize, &osdysize);

    x = ca->x >> 2;
    b = ca->buf;
    addr.l =  x + (osdxsize/4 * (unsigned long) ca->y);

    /* render */
    for (h = 0; h < ca->height; h++) {
        while (sram_busy);
        CS_LOW;
        sram_byteo_sqi(SRAM_WRITE);
        sram_byteo_sqi(addr.b2);
        sram_byteo_sqi(addr.b1);
        sram_byteo_sqi(addr.b0);
        b = copy_line(b, ca->rwidth);
        CS_HIGH;
        addr.l += osdxsize/4;
    }
}


void init_video(void)
{
    video_init_sram();
    video_init_hw();

    params_add(params_video);
    if (hw_rev < 0x03)
        params_add(params_video0v1);
    else
        params_add(params_video0v3);
    process_add(render_process);
}


/* line timer */
void __attribute__((__interrupt__, auto_psv )) _T2Interrupt()
{
    /* stop timer */
    T2CONbits.TON = 0;
    _T2IF = 0;
    
    if (PR2 != LINE_TMR) {
        if (hw_rev <= 0x02)
            OE_RAM = 0;
        else
            OE_RAM2 = 0;
        SPI2STATbits.SPIEN = 1;
        PR2 = LINE_TMR;
        T2CONbits.TON = 1;
    } else {
        if (hw_rev <= 0x02) {
            OE_RAM = 1;
        } else {
            OE_RAM2 = 1;
            TRISA &= 0xfff3;
            TRISA |= 0xc;
        }
        CS_HIGH;
        SRAM_OUT;
        if (hw_rev == 0x03)
            _RP56R = 0;
        else
            _RP54R = 0;
        SPI2STATbits.SPIEN = 0;
    }
}


static void render_line(void)
{
    static union sram_addr addr __attribute__((aligned(2)));
    static unsigned int osdxsize;
    static unsigned int x_offset;
    static unsigned int last_line = 200;
    
    line++;

    if (line < cfg->y_offset-2) {
        /* do nothing */
    } else if (line < cfg->y_offset-1) {
        /* setup vars */
        osdxsize = video_xsizes[cfg->x_size_id].xsize;

        /* calc last_line */
        last_line = cfg->y_size + cfg->y_offset;

        /* avoid sram_busy soft-locks */
        if (last_line > last_line_cnt - 10)
            last_line = last_line_cnt - 10;

        /* auto detect video standard */
#if 0
        if (last_line_cnt < 300)
            cfg->mode |= VIDEO_MODE_STANDARD_MASK;
        else
            cfg->mode &= ~VIDEO_MODE_STANDARD_MASK;
#endif
        
        sram_busy = 1;
        addr.l = 0;

        if ((videocore_ctrl & CTRL_COMPSYNC) == 0) {
            if (int_sync_cnt < CNT_INT_MODE)
                odd = PORTBbits.RB15;
        }
            
        if (cfg->mode & VIDEO_MODE_SCAN_MASK) {
            if (odd == 0) {
                addr.l += (osdxsize/4);
            }
        }
    } else if (line < cfg->y_offset) {
        sram_exit_sqi();
        /* make sure we are in sequential mode */
        CS_LOW;
        sram_byte_spi(SRAM_WMODE);
        sram_byte_spi(0x40);
        CS_HIGH;
        /* switch sram to sdi mode */
        CS_LOW;
        sram_byte_spi(SRAM_DIO);
        CS_HIGH;
        SRAM_OUT;

        x_offset = cfg->x_offset;
        if (int_sync_cnt > CNT_INT_MODE) {
            x_offset += INT_X_OFFSET;
            if (hw_rev < 0x03)
                x_offset -= 80;
        }

    } else if (line < last_line) {
        /* render */
        CS_LOW;
        sram_byteo_sdi(SRAM_READ);
        sram_byteo_sdi(addr.b2);
        sram_byteo_sdi(addr.b1);
        sram_byteo_sdi(addr.b0);
        SRAM_IN;
        CLK_HIGH; CLK_LOW;
        CLK_HIGH; CLK_LOW;
        CLK_HIGH; CLK_LOW;
        CLK_HIGH; CLK_LOW;
        if (hw_rev == 0x03)
            _RP56R = SCK2_O;
        else
            _RP54R = SCK2_O;

        /* line start timer - x_offset */
        PR2 = x_offset * 5;
        T2CONbits.TON = 1;

        /* calc next address */
        if (cfg->mode & VIDEO_MODE_SCAN_MASK) {
            addr.l += (unsigned long) ((osdxsize/4) * 2);
        } else {
            addr.l += (unsigned long) (osdxsize/4);
        }
    } else if (line == last_line ) {
        /* switch sram back to sqi mode */
        sram_exit_sdi();
        CS_LOW;
        sram_byte_spi(SRAM_QIO);
        CS_HIGH;
        SRAM_OUTQ;
        sram_busy = 0;
    }
}


/* external sync */
void __attribute__((__interrupt__, auto_psv )) _INT1Interrupt()
{
    last_line_cnt = line;
    line = 0;
    int_sync_cnt = 0;
    _INT2IF = 0;
    _INT2IE = 1;
    _INT1IF = 0;
    _T4IP = 3;
}
void __attribute__((__interrupt__, auto_psv )) _INT2Interrupt()
{
    render_line();
    _INT2IF = 0;
}

void __attribute__((__interrupt__, no_auto_psv )) _T5Interrupt()
{
    T5CONbits.TON = 0;
    IC1CON1bits.ICM = 1;
    _T5IF = 0;
}

/* comparator + input capture sync */
void __attribute__((__interrupt__, auto_psv )) _IC1Interrupt(void)
{
    static unsigned int tp = 0xffff, last_cnt;
    static unsigned char vsync = 0;
    unsigned int cnt, t;
    unsigned char std = 0;

    /* empty fifo */
    do {
        cnt = IC1BUF;
    } while (IC1CON1bits.ICBNE != 0);
    IFS0bits.IC1IF = 0;
    
    t = cnt - last_cnt;
    if (t < 100)
        return;
    
    last_cnt = cnt;

    if (PORTCbits.RC6) {
        /* rising edge */
        /* vsync detector */
        if (abs(((long) t) - 1900) < 200)
            tp = (tp << 1) | 1;
        else if (abs(((long) t) - 170) < 70)
            tp = (tp << 1);
        else {
            tp = 0xffff;
        }

        switch (tp) {
            case 0b1000001111100000:
                std = 1; // pal
                break;
            case 0b0000111111000000:
                std = 2; // ntsc
                break;
            default:
                std = 0;
                break;
        }
        
        if (std > 0) {
            /* pull downs - input video */
            _CNPUA2 = 0;
            _CNPDA2 = 1;
            _CNPUA3 = 0;
            _CNPDA3 = 1;
            _T4IP = 3;
            vsync = std;
            last_line_cnt = line+10;
            line = 10;
            odd = 0;
            int_sync_cnt = 0;
            tp = 0xffff;
            return;
        }

        if (vsync) {
            if (abs(((long) t) - 329) < 100) {
                if (((vsync == 1) && (line < 314)) ||
                    ((vsync == 2) && (line < 264))) {
                    IC1CON1bits.ICM = 0;
                    TMR5 = 0;
                    T5CONbits.TON = 1;
                }
                render_line();
            } else {
                /* lost sync */
                vsync = 0;
            }
        }
    } else {
        /* falling edge */
        if (vsync && (line == 10))
            if (abs(((long) t) - 2050) < 100)
                odd = 1;

    }
}


/* internal sync generator */
void __attribute__((__interrupt__, auto_psv )) _T4Interrupt()
{
    static unsigned char cnt;

    _T4IF = 0;

    if (int_sync_cnt < CNT_INT_MODE) {
        /* ext sync */
        int_sync_cnt++;
    } else if (int_sync_cnt < CNT_INT_MODE + 1) {
        /* prepare internal sync */
        last_line_cnt = 312;
        line = 0;
        odd = 1;
        int_sync_cnt++;
        if (hw_rev < 0x03) {
            _INT2IE = 0;
        } else {
            /* pull ups - black level */
            _CNPDA2 = 0;
            _CNPUA2 = 1;
            _CNPDA3 = 0;
            _CNPUA3 = 1;
        }
        cnt = 0;
        _T4IP = 5;
    } else {
        /* internal sync */
        cnt++;
        
        if (hw_rev <= 0x02) {
            if (odd == 1) {
                if (line < 2) {
                    /* vsync sync pulses */
                    if ((cnt == 1) || (cnt == 6)) {
                        _LATA9 = 0;
                    } else if ((cnt == 5) || (cnt == 10)) {
                        _LATA9 = 1;
                    }
                } else if (line < 3) {
                    if ((cnt == 1) || (cnt == 6)) {
                        _LATA9 = 0;
                    } else if ((cnt == 5) || (cnt == 7)) {
                        _LATA9 = 1;
                    }
                } else if ((line < 5) || (line > 309)) {
                    if ((cnt == 1) || (cnt == 6)) {
                        _LATA9 = 0;
                    } else if ((cnt == 2) || (cnt == 7)) {
                        _LATA9 = 1;
                    }
                } else {
                    /* normal sync pulse */
                    if (cnt == 1) {
                        _LATA9 = 0;
                    } else if (cnt == 2) {
                        _LATA9 = 1;
                    }
                }
            } else {
                if (line < 1) {
                    /* vsync sync pulses */
                    if ((cnt == 1) || (cnt == 6)) {
                        _LATA9 = 0;
                    } else if ((cnt == 2) || (cnt == 10)) {
                        _LATA9 = 1;
                    }
                } else if (line < 3) {
                    if ((cnt == 1) || (cnt == 6)) {
                        _LATA9 = 0;
                    } else if ((cnt == 5) || (cnt == 10)) {
                        _LATA9 = 1;
                    }

                } else if ((line < 5) || (line > 308)) {
                    if ((cnt == 1) || (cnt == 6)) {
                        _LATA9 = 0;
                    } else if ((cnt == 2) || (cnt == 7)) {
                        _LATA9 = 1;
                    }
                } else {
                    /* normal sync pulse */
                    if (cnt == 1) {
                        _LATA9 = 0;
                    } else if (cnt == 2) {
                        _LATA9 = 1;
                    }
                }
            }
        } else {
            if (odd == 1) {
                if (line < 2) {
                    /* vsync sync pulses */
                    if ((cnt == 1) || (cnt == 6)) {
                        _LATA4 = 0;
                    } else if ((cnt == 5) || (cnt == 10)) {
                        _LATA4 = 1;
                    }
                } else if (line < 3) {
                    if ((cnt == 1) || (cnt == 6)) {
                        _LATA4 = 0;
                    } else if ((cnt == 5) || (cnt == 7)) {
                        _LATA4 = 1;
                    }
                } else if ((line < 5) || (line > 309)) {
                    if ((cnt == 1) || (cnt == 6)) {
                        _LATA4 = 0;
                    } else if ((cnt == 2) || (cnt == 7)) {
                        _LATA4 = 1;
                    }
                } else {
                    /* normal sync pulse */
                    if (cnt == 1) {
                        _LATA4 = 0;
                    } else if (cnt == 2) {
                        _LATA4 = 1;
                    }
                }
            } else {
                if (line < 1) {
                    /* vsync sync pulses */
                    if ((cnt == 1) || (cnt == 6)) {
                        _LATA4 = 0;
                    } else if ((cnt == 2) || (cnt == 10)) {
                        _LATA4 = 1;
                    }
                } else if (line < 3) {
                    if ((cnt == 1) || (cnt == 6)) {
                        _LATA4 = 0;
                    } else if ((cnt == 5) || (cnt == 10)) {
                        _LATA4 = 1;
                    }
                } else if ((line < 5) || (line > 308)) {
                    if ((cnt == 1) || (cnt == 6)) {
                        _LATA4 = 0;
                    } else if ((cnt == 2) || (cnt == 7)) {
                        _LATA4 = 1;
                    }
                } else {
                    /* normal sync pulse */
                    if (cnt == 1) {
                        _LATA4 = 0;
                    } else if (cnt == 2) {
                        _LATA4 = 1;
                    }
                }
            }
        }
        if (cnt == 1)
            render_line();

        if (cnt > 9) {
            cnt = 0;
            if (line == 312) {
                last_line_cnt = line;
                line = 0;
                odd = odd ^ 1;
            }
        }
    }
}


static void shell_cmd_stats(char *args, void *data)
{
    unsigned char dac_status[VIDEO_DAC_BUF_SIZE], i;
    shell_printf("\nVideo config:\n");
    shell_printf(" standard=%s,%s\n",
        (cfg->mode & VIDEO_MODE_STANDARD_MASK) != 0 ? "ntsc" : "pal",
        (cfg->mode & VIDEO_MODE_SCAN_MASK) != 0 ? "interlaced" : "progressive");
    shell_printf(" internal sync=%u\n", (cfg->mode & VIDEO_MODE_SYNC_MASK) != 0 ? 1 : 0);
    if (hw_rev < 0x03) {
        shell_printf(" brightness=%u\n", cfg->brightness);
    } else {
        shell_printf(" levels: white=%u gray=%u %black=%u\n",
                cfg->white_lvl,
                cfg->gray_lvl,
                cfg->black_lvl);
        
        shell_printf(" dac settings:\n  ");
        video_read_dac(dac_status);
        for (i = 0; i < VIDEO_DAC_BUF_SIZE; i++) {
            shell_printf("0x%02x ", dac_status[i]);
            if (((i+1) % 3) == 0)
                printf("\n  ");
        }
        shell_printf("\n");
    }

    shell_printf(" offset: x=%u y=%u\n",
            cfg->x_offset, cfg->y_offset);
    shell_printf(" size: x=%u y=%u\n",
            video_xsizes[cfg->x_size_id].xsize, cfg->y_size);
    
    shell_printf("\nVideocore stats:\n");
    shell_printf(" scratchpad memory: A=%u/%u B=%u/%u\n",
                scratchpad[0].alloc_size, SCRATCHPAD_SIZE,
                scratchpad[1].alloc_size, SCRATCHPAD_SIZE);
    shell_printf(" canvas fifo: size=%u peak=%u max=%u\n",
                canvas_pipe.pwr - canvas_pipe.prd, canvas_pipe.peak, MAX_CANVAS_PIPE_MASK+1);
    shell_printf(" status: last_line_cnt=%u sram_busy=%u int_sync_cnt=%u\n",
                last_line_cnt, sram_busy, int_sync_cnt);
}

#define SHELL_CMD_CONFIG_ARGS   10
static void shell_cmd_config(char *args, void *data)
{
    struct shell_argval argval[SHELL_CMD_CONFIG_ARGS+1];
    unsigned char t, i;
    int int_var;
    float f;
    
    t = shell_arg_parser(args, argval, SHELL_CMD_CONFIG_ARGS);

    if (t < 1) {
        shell_printf("\narguments:\n");
        shell_printf(" -s <standard>    video standard: [p]al or [n]tsc\n");
        shell_printf(" -m <scan_mode>   video scan mode: [p]rogressive or [i]nterlaced\n");
        shell_printf(" -i <int_sync>    internal sync: 0 or 1\n");

        if (hw_rev < 0x03) {
            shell_printf(" -t <brightness>  brightness: 0 (max) to 1000 (min)\n");
        } else {
            shell_printf(" -w <white_lvl>   white voltage level: 0 to \n");
            shell_printf(" -g <gray_lvl>    gray voltage level: 0 to \n");
            shell_printf(" -b <black_lvl>   black voltage level: 0 to \n");
        }
        shell_printf(" -x <x_size>    horizontal video resolution:");
        for (i = 0; i < VIDEO_XSIZE_END; i++)
            shell_printf(" %d", video_xsizes[i].xsize);
        shell_printf("\n -y <y_size>    vertical video resolution\n");
        shell_printf(" -h <x_offset>  horizontal video offset\n");
        shell_printf(" -v <y_offset>  vertical video offset\n");
        
    } else {
        for (i = 0; i < t; i++) {
            switch (argval[i].key) {
                case 's':
                    if (strcmp(argval[i].val, "p") == 0)
                        cfg->mode &= ~VIDEO_MODE_STANDARD_MASK;
                    else
                        cfg->mode |= VIDEO_MODE_STANDARD_MASK;
                case 'm':
                    if (strcmp(argval[i].val, "p") == 0)
                        cfg->mode &= ~VIDEO_MODE_SCAN_MASK;
                    else
                        cfg->mode |= VIDEO_MODE_SCAN_MASK;
                    break;
                case 'i':
                    if (strcmp(argval[i].val, "0") == 0)
                        cfg->mode &= ~VIDEO_MODE_SYNC_MASK;
                    else
                        cfg->mode |= VIDEO_MODE_SYNC_MASK;
                    break;

                case 't':
                    int_var = atoi(argval[i].val);
                    cfg->brightness = (unsigned int) int_var;
                    break;

                case 'w':
                    int_var = atoi(argval[i].val);
                    cfg->white_lvl = (unsigned char) int_var;
                    break;
                case 'g':
                    int_var = atoi(argval[i].val);
                    cfg->gray_lvl = (unsigned char) int_var;
                    break;
                case 'b':
                    int_var = atoi(argval[i].val);
                    cfg->black_lvl = (unsigned char) int_var;
                    break;
                    
                case 'x':
                    int_var = atoi(argval[i].val);
                    for (i = 0; i < VIDEO_XSIZE_END; i++) {
                        if (int_var == video_xsizes[i].xsize) {
                            cfg->x_size_id = i;
                            break;
                        }
                    }
                    break;
                case 'y':
                    int_var = atoi(argval[i].val);
                    cfg->y_size = (unsigned int) int_var;
                    break;
                case 'h':
                    int_var = atoi(argval[i].val);
                    cfg->x_offset = (unsigned int) int_var;
                    break;
                case 'v':
                    int_var = atoi(argval[i].val);
                    cfg->y_offset = (unsigned int) int_var;
                    break;

                case 'c':
                    int_var = atoi(argval[i].val);
                    CVR1CONbits.CVRR = (unsigned int) int_var & 0x1;
                    CVR1CONbits.CVRR1 = (unsigned int) (int_var >> 1) & 0x1;
                    break;
                case 'r':
                    int_var = atoi(argval[i].val);
                    CVR1CONbits.CVR = (unsigned int) int_var & 0xf;
                    
                    int_var = CVR1CONbits.CVRR | (CVR1CONbits.CVRR1 << 1);
                    
                    f = (float) CVR1CONbits.CVR;
                    
                    switch (int_var) {
                        case 0:
                            f = f/32.0 * 3.3 + (1/4.0) * 3.3;
                            break;
                        case 1:
                            f = f /24.0 * 3.3;
                            break;
                        case 2:
                            f = f/24.0 * 3.3 + (1/3.0) * 3.3;
                            break;
                        case 3:
                            f = f / 16.0 * 3.3;
                            break;
                        default:
                            f = 0;
                            break;
                    }
                    shell_printf("\nvref=%0.3f\n", f);
                    break;
                case 'l':
                    int_var = atoi(argval[i].val);
                    PR5 = (unsigned int) int_var;
                    break;
                default:
                    break;
            }
        }
        video_apply_config_cbk();
    }
}


static const struct shell_cmdmap_s video_cmdmap[] = {
    {"config", shell_cmd_config, "Configure video settings", SHELL_CMD_SIMPLE},
    {"stats", shell_cmd_stats, "Display statistics", SHELL_CMD_SIMPLE},
    {"", NULL, ""},
};

void shell_cmd_video(char *args, void *data)
{
    shell_exec(args, video_cmdmap, data);
}
