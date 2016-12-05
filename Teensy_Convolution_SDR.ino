/*********************************************************************************************
 * (c) Frank DD4WH 2016_12_04
 *  
 * "Teensy Convolution SDR" 
 * 
 * FFT Fast Convolution = Digital Convolution
 * with overlap - save = overlap-discard
 * dynamically creates FIR coefficients  
 * 
 * in floating point 32bit 
 * tested on Teensy 3.6
 * 
 * Part of the evolution of this project has been documented here:
 * https://forum.pjrc.com/threads/40188-Fast-Convolution-filtering-in-floating-point-with-Teensy-3-6/page2
 * 
 * parts of the code modified from and/or inspired by
 * Teensy SDR (rheslip & DD4WH): https://github.com/DD4WH/Teensy-SDR-Rx
 * mcHF (KA7OEI, DF8OE, DB4PLE): https://github.com/df8oe/mchf-github/
 * libcsdr (András Retzler): https://github.com/simonyiszk/csdr 
 * sample-rate-change-on-the-fly code by Frank Bösing
 * 
 * GREAT THANKS FOR ALL THE HELP AND INPUT BY WALTER, WMXZ ! 
 * Audio queue optimized by Pete El Supremo 2016_10_27, thanks Pete!
 * An important hint on the implementation came from Alberto I2PHD, thanks for that!
 * 
 * Audio processing in float32_t with the NEW ARM CMSIS lib (see https://forum.pjrc.com/threads/38325-Excellent-results-with-Floating-Point-FFT-IFFT-Processing-and-Teensy-3-6?p=119797&viewfull=1#post119797),
 * and see here: https://forum.pjrc.com/threads/38325-Excellent-results-with-Floating-Point-FFT-IFFT-Processing-and-Teensy-3-6?p=120177&viewfull=1#post120177 
 * MIT license
 */

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Metro.h>
#include "font_Arial.h"
#include <ILI9341_t3.h>
#include <arm_math.h>
#include <arm_const_structs.h>
#include <si5351.h>
#include <Encoder.h>
#include <EEPROM.h>
#include <Bounce.h>

long calibration_factor = 10000000 ;// 10002285;
long calibration_constant = 3500;
unsigned long long hilfsf;

// Optical Encoder connections
Encoder tune(16, 17);
Si5351 si5351;
#define MASTER_CLK_MULT  4  // QSD frontend requires 4x clock

#define BACKLIGHT_PIN 0
#define TFT_DC      20
#define TFT_CS      21
#define TFT_RST     32  // 255 = unused. connect to 3.3V
#define TFT_MOSI     7
#define TFT_SCLK    14
#define TFT_MISO    12

ILI9341_t3 tft = ILI9341_t3(TFT_CS, TFT_DC, TFT_RST, TFT_MOSI, TFT_SCLK, TFT_MISO);

// push-buttons
#define   BUTTON_1_PIN      33
#define   BUTTON_2_PIN      34
#define   BUTTON_3_PIN      35
#define   BUTTON_4_PIN      36
#define   BUTTON_5_PIN      37

Bounce button1 = Bounce(BUTTON_1_PIN, 50); //
Bounce button2 = Bounce(BUTTON_2_PIN, 50); //
Bounce button3 = Bounce(BUTTON_3_PIN, 50);//
Bounce button4 = Bounce(BUTTON_4_PIN, 50);//
Bounce button5 = Bounce(BUTTON_5_PIN, 50); // 

Metro five_sec=Metro(2000); // Set up a Metro

// this audio comes from the codec by I2S2
AudioInputI2S            i2s_in; 
           
AudioRecordQueue         Q_in_L;    
AudioRecordQueue         Q_in_R;    

AudioPlayQueue           Q_out_L; 
AudioPlayQueue           Q_out_R; 
AudioOutputI2S           i2s_out;           

AudioConnection          patchCord1(i2s_in, 0, Q_in_L, 0);
AudioConnection          patchCord2(i2s_in, 1, Q_in_R, 0);

AudioConnection          patchCord3(Q_out_L, 0, i2s_out, 1);
AudioConnection          patchCord4(Q_out_R, 0, i2s_out, 0);

AudioControlSGTL5000     sgtl5000_1;     //xy=265.212

int idx_t = 0;
int idx = 0;
int64_t sum;
float32_t mean;
int n_L;
int n_R;
long int n_clear;

float32_t FFT_magn[4096];
float32_t FFT_spec[256];
float32_t FFT_spec_old[256];
int16_t pixelnew[256];
int16_t pixelold[256];
float32_t LPF_spectrum = 0.1;
#define SPECTRUM_ZOOM_1          16
#define SPECTRUM_ZOOM_2           8
#define SPECTRUM_ZOOM_4           4
#define SPECTRUM_ZOOM_8           2
#define SPECTRUM_ZOOM_16          1
     
uint32_t spectrum_zoom = SPECTRUM_ZOOM_1;


//////////////////////////////////////
// constants for display
//////////////////////////////////////
int spectrum_y = 120; // upper edge
int spectrum_x = 10;
int spectrum_height = 90;
int spectrum_pos_centre_f = 64;
// Text and position for the FFT spectrum display scale

#define SAMPLE_RATE_MIN               0
#define SAMPLE_RATE_8K                0
#define SAMPLE_RATE_11K               1
#define SAMPLE_RATE_16K               2  
#define SAMPLE_RATE_22K               3
#define SAMPLE_RATE_32K               4
#define SAMPLE_RATE_44K               5
#define SAMPLE_RATE_48K               6
#define SAMPLE_RATE_88K               7
#define SAMPLE_RATE_96K               8
#define SAMPLE_RATE_176K              9
#define SAMPLE_RATE_192K              10
#define SAMPLE_RATE_MAX               10

uint8_t sr =                     SAMPLE_RATE_96K;
uint8_t SAMPLE_RATE =            SAMPLE_RATE_96K; 

typedef struct SR_Descriptor
{
    const uint8_t SR_n;
    const uint32_t rate;
    const char* const text;
    const char* const f1;
    const char* const f2;
    const char* const f3;
    const char* const f4;
    const float32_t x_factor;
    const uint8_t x_offset;
} SR_Desc;
const SR_Descriptor SR [SAMPLE_RATE_MAX + 1] =
{
    //   SR_n , rate, text, f1, f2, f3, f4, x_factor = pixels per f1 kHz in spectrum display
    {  SAMPLE_RATE_8K, 8000,  "  8k", " 1", " 2", " 3", " 4", 64.0, 11}, // not OK
    {  SAMPLE_RATE_11K, 11025, " 11k", " 1", " 2", " 3", " 4", 43.1, 17}, // not OK
    {  SAMPLE_RATE_16K, 16000, " 16k",  " 4", " 4", " 8", "12", 64.0, 1}, // OK
    {  SAMPLE_RATE_22K, 22050, " 22k",  " 5", " 5", "10", "15", 58.05, 6}, // OK 
    {  SAMPLE_RATE_32K, 32000,  " 32k", " 5", " 5", "10", "15", 40.0, 24}, // OK, one more indicator? 
    {  SAMPLE_RATE_44K, 44100,  " 44k", "10", "10", "20", "30", 58.05, 6}, // OK
    {  SAMPLE_RATE_48K, 48000,  " 48k", "10", "10", "20", "30", 53.33, 11}, // OK
    {  SAMPLE_RATE_88K, 88200,  " 88k", "20", "20", "40", "60", 58.05, 6}, // OK
    {  SAMPLE_RATE_96K, 96000,  " 96k", "20", "20", "40", "60", 53.33, 12}, // OK 
    {  SAMPLE_RATE_176K, 176000,  "176k", "20", "40", "60", "80", 58.05, 17}, // not OK
    {  SAMPLE_RATE_192K, 192000,  "192k", "20", "40", "60", "80", 53.33, 11} // not OK
};    
int16_t IF_FREQ = SR[SAMPLE_RATE].rate / 4;     // IF (intermediate) frequency
#define F_MAX 36000000 + IF_FREQ
#define F_MIN 12000 + IF_FREQ

ulong samp_ptr = 0;

const int myInput = AUDIO_INPUT_LINEIN;

#define BUFFER_SIZE 128

// complex FFT with the new library CMSIS V4.5
const static arm_cfft_instance_f32 *S;

// create coefficients on the fly for custom filters
// and let the algorithm define which FFT size you need
// input variables by the user
// Fpass, Fstop, Astop (stopband attenuation)  
// fixed: sample rate, scale = 1.0 ???
// tested & working samplerates (processor load):
// 96k (46%), 88.2k, 48k, 44.1k (18%), 32k (13.6%),

float32_t LP_Fpass = 6200;
uint32_t LP_F_help = 6200;
float32_t LP_Fstop = 6220;
float32_t LP_Astop = 90;
float32_t LP_Fpass_old = 0.0;

float32_t FIR_Coef[2049];
#define MAX_NUMCOEF 2049
#define TPI           6.28318530717959f
#define PIH           1.57079632679490f
uint32_t m_NumTaps = 3;
const uint32_t FFT_L = 4096;
uint32_t FFT_length = FFT_L;
uint32_t N_BLOCKS;
const uint32_t N_B = FFT_L / 2 / BUFFER_SIZE; 
float32_t float_buffer_L [BUFFER_SIZE * N_B];
float32_t float_buffer_R [BUFFER_SIZE * N_B];
float32_t FFT_buffer [FFT_L * 2] __attribute__ ((aligned (4)));
float32_t last_sample_buffer_L [BUFFER_SIZE * N_B];
float32_t last_sample_buffer_R [BUFFER_SIZE * N_B];

// complex iFFT with the new library CMSIS V4.5
const static arm_cfft_instance_f32 *iS;
float32_t iFFT_buffer [FFT_L * 2] __attribute__ ((aligned (4)));

// FFT instance for direct calculation of the filter mask
// from the impulse response of the FIR - the coefficients
const static arm_cfft_instance_f32 *maskS;
float32_t FIR_filter_mask [FFT_L * 2] __attribute__ ((aligned (4))); 

//////////////////////////////////////
// for frequency display
//////////////////////////////////////
uint8_t freq_flag = 0;
uint8_t digits_old [] = {9,9,9,9,9,9,9,9,9,9};
#define pos 50 // position of spectrum display, has to be < 124
#define pos_version 119 // position of version number printing
#define pos_x_tunestep 100
#define pos_y_tunestep 119 // = pos_y_menu 91
#define pos_x_frequency 5 //21 //100
#define pos_y_frequency 52 //61  //119
#define pos_x_smeter 5
#define pos_y_smeter 94
#define s_w 10
#define notchpos 2
#define notchL 15
#define notchColour YELLOW
int pos_centre_f = 98; // 
#define spectrum_span 44.118
#define font_width 16
uint8_t sch = 0;

#define       DEMOD_MIN           0
#define       DEMOD_USB           0
#define       DEMOD_LSB           1
#define       DEMOD_AM            2
#define       DEMOD_STEREO_LSB    3 // LSB, with pseude-stereo effect
#define       DEMOD_STEREO_USB    4 // USB, with pseude-stereo effect
#define       DEMOD_DSB           5 // double sideband: SSB without eliminating the opposite sideband
#define       DEMOD_STEREO_DSB    6 // double sideband: SSB without eliminating the opposite sideband
#define       DEMOD_SAM           7 // synchronous AM demodulation
#define       DEMOD_MAX           4
#define BAND_LW     0   
#define BAND_MW     1
#define BAND_120M   2
#define BAND_90M    3
#define BAND_75M    4    
#define BAND_60M    5
#define BAND_49M    6
#define BAND_41M    7
#define BAND_31M    8
#define BAND_25M    9
#define BAND_22M   10
#define BAND_19M   11
#define BAND_16M   12
#define BAND_15M   13
#define BAND_13M   14
#define BAND_11M   15

#define FIRST_BAND BAND_LW
#define LAST_BAND  BAND_15M
#define NUM_BANDS  16
#define STARTUP_BAND BAND_MW    // 

struct band {
  long freq; // frequency in Hz
//  unsigned long freq; // frequency in Hz
  String name; // name of band
  int mode; 
  int bandwidthU;
  int bandwidthL;
  int RFgain; 
};
// f, band, mode, bandwidth, RFgain
struct band bands[NUM_BANDS] = {
//  77500+IF_FREQ,"VLF", DEMOD_AM, 3600,3600,0,  
  225000+IF_FREQ,"LW", DEMOD_AM, 3600,3600,0,
  531000 + IF_FREQ,"MW",  DEMOD_AM, 3600,3600,0,
  2485000+IF_FREQ,"120M",  DEMOD_AM, 3600,3600,0,
  3500000+IF_FREQ,"90M",  DEMOD_LSB, 3600,3600,6,
  3905000+IF_FREQ,"75M",  DEMOD_AM, 3600,3600,4,
  5025000+IF_FREQ,"60M",  DEMOD_AM, 3600,3600,7,
  5981000 + IF_FREQ,"49M",  DEMOD_AM, 3600,3600,0,
  7120000+IF_FREQ,"41M",  DEMOD_AM, 3600,3600,0,
  9420000+IF_FREQ,"31M",  DEMOD_AM, 3600,3600,0,
  11735000+IF_FREQ,"25M", DEMOD_AM, 3600,3600,0,
  13570000+IF_FREQ,"22M", DEMOD_AM, 3600,3600,0,
  15140000+IF_FREQ,"19M",  DEMOD_AM, 3600,3600,0,
  17480000+IF_FREQ,"16M", DEMOD_AM, 3600,3600,0,
  18900000+IF_FREQ,"15M", DEMOD_AM, 3600,3600,0,
  21450000+IF_FREQ,"13M", DEMOD_AM, 3600,3600,0,
  25670000+IF_FREQ,"11M", DEMOD_AM, 3600,3600,0
};
int band = STARTUP_BAND;

#define TUNE_STEP_MIN   0
#define TUNE_STEP1   0    // shortwave
#define TUNE_STEP2   1   // fine tuning
#define TUNE_STEP3   2    //
#define TUNE_STEP_MAX 2 
#define first_tunehelp 1
#define last_tunehelp 3
uint8_t tune_stepper = 0;
int tunestep = 5000 / 4; //TUNE_STEP1;
String tune_text = "Fast Tune";
 
int8_t first_block = 1;
uint32_t i = 0;
int32_t FFT_shift = 2048; // which means 1024 bins!
//int32_t FFT_shift = 1024;

//int16_t FFT_bin[256];

// used for AM demodulation
float32_t audiotmp = 0.0f;
float32_t w = 0.0f;
float32_t wold = 0.0f;
float32_t last_dc_level = 0.0f;
uint8_t audio_flag = 1;

typedef struct DEMOD_Descriptor
{   const uint8_t DEMOD_n;
    const char* const text;
} DEMOD_Desc;
const DEMOD_Descriptor DEMOD [8] =
{
    //   DEMOD_n, name
    {  DEMOD_USB, " USB  "}, 
    {  DEMOD_LSB, " LSB  "}, 
    {  DEMOD_AM,  "  AM  "}, 
    {  DEMOD_STEREO_LSB, "StLSB "}, 
    {  DEMOD_STEREO_USB, "StUSB "}, 
    {  DEMOD_DSB, " DSB  "}, 
    {  DEMOD_STEREO_DSB, "StDSB "},
    {  DEMOD_SAM, " SAM  "},
};

uint8_t       demod_mode =        DEMOD_AM;

/****************************************************************************************
 *  init IIR filters
 ****************************************************************************************/
float32_t coefficient_set[5] = {0, 0, 0, 0, 0};
// 2-pole biquad IIR - definitions and Initialisation
const uint32_t N_stages_biquad_lowpass1 = 1;
float32_t biquad_lowpass1_state [N_stages_biquad_lowpass1 * 4];
float32_t biquad_lowpass1_coeffs[5 * N_stages_biquad_lowpass1] = {0,0,0,0,0}; 
arm_biquad_casd_df1_inst_f32 biquad_lowpass1;

void setup() {
  Serial.begin(115200);
  delay(1000);

  // for the large FFT sizes we need a lot of buffers
  AudioMemory(100);

  // Enable the audio shield. select input. and enable output
  sgtl5000_1.enable();
  sgtl5000_1.inputSelect(myInput);
  sgtl5000_1.volume(0.6); // when I put this to 0.9, the digital noise of the Teensy is way too loud,
  // so 0.5 to 0.65 seems a good compromise
  sgtl5000_1.adcHighPassFilterDisable(); // does not help too much!
  sgtl5000_1.lineInLevel(11);

  pinMode( BACKLIGHT_PIN, OUTPUT );
  analogWrite( BACKLIGHT_PIN, 1023 );
  pinMode(BUTTON_1_PIN, INPUT_PULLUP);
  pinMode(BUTTON_2_PIN, INPUT_PULLUP);
  pinMode(BUTTON_3_PIN, INPUT_PULLUP);
  pinMode(BUTTON_4_PIN, INPUT_PULLUP);
  pinMode(BUTTON_5_PIN, INPUT_PULLUP);

  tft.begin();
  tft.setRotation( 3 );
  tft.fillScreen(ILI9341_BLACK);
  tft.setCursor(10, 1);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_ORANGE);
  tft.setFont(Arial_14);
  tft.print("Teensy Convolution SDR ");
  tft.setFont(Arial_10);
  prepare_spectrum_display();

 /****************************************************************************************
 *  set filter bandwidth
 ****************************************************************************************/

    LP_F_help = analogRead(15) * 10.0;
    LP_F_help = (int)(LP_F_help/100 * 100);
    LP_F_help += 100;
    LP_Fpass_old = LP_F_help;
  // this routine does all the magic of calculating the FIR coeffs (Bessel-Kaiser window)
    calc_FIR_coeffs (FIR_Coef, 2049, (float32_t)LP_F_help, LP_Astop, 0, 0.0, SR[SAMPLE_RATE].rate);
    FFT_length = 4096;
    m_NumTaps = 2049;
    N_BLOCKS = FFT_length / 2 / BUFFER_SIZE;

  // set to zero all other coefficients in coefficient array    
  for(i = 0; i < MAX_NUMCOEF; i++)
  {
//    Serial.print (FIR_Coef[i]); Serial.print(", ");
      if (i >= m_NumTaps) FIR_Coef[i] = 0.0;
  }
    
 /****************************************************************************************
 *  init complex FFTs
 ****************************************************************************************/
      S = &arm_cfft_sR_f32_len4096;
      iS = &arm_cfft_sR_f32_len4096;
      maskS = &arm_cfft_sR_f32_len4096;

 /****************************************************************************************
 *  Calculate the FFT of the FIR filter coefficients once to produce the FIR filter mask
 ****************************************************************************************/
    init_filter_mask();
 
 /****************************************************************************************
 *  show Filter response
 ****************************************************************************************/
    setI2SFreq (8000);
//    SAMPLE_RATE = 8000;
    delay(200);
    for(int y=0; y < FFT_length * 2; y++)
    FFT_buffer[y] = 180 * FIR_filter_mask[y];
    calc_spectrum_mags(16,0.2);
    show_spectrum();
    delay(1000);
    SAMPLE_RATE = sr;

 /****************************************************************************************
 *  Set sample rate
 ****************************************************************************************/
    setI2SFreq (SR[SAMPLE_RATE].rate);
    delay(200); // essential ?

    biquad_lowpass1.numStages = N_stages_biquad_lowpass1; // set number of stages
    biquad_lowpass1.pCoeffs = biquad_lowpass1_coeffs; // set pointer to coefficients file
    for(i = 0; i < 4 * N_stages_biquad_lowpass1; i++)
    {
        biquad_lowpass1_state[i] = 0.0; // set state variables to zero   
    }
    biquad_lowpass1.pState = biquad_lowpass1_state; // set pointer to the state variables
 
  /****************************************************************************************
 *  set filter bandwidth of IIR filter according to pot setting
 ****************************************************************************************/
 // also adjust IIR AM filter
    // calculate IIR coeffs
    set_IIR_coeffs ((float32_t)LP_F_help / 2.0, 1.3, (float32_t)SR[SAMPLE_RATE].rate, 0); // 1st stage
    for(i = 0; i < 5; i++)
    {   // fill coefficients into the right file
        biquad_lowpass1_coeffs[i] = coefficient_set[i];
    }
    show_bandwidth (demod_mode, LP_F_help);

 /****************************************************************************************
 *  start local oscillator Si5351
 ****************************************************************************************/
  si5351.init(SI5351_CRYSTAL_LOAD_10PF, 27000000, calibration_constant);
  setfreq();
  delay(100); 
  show_frequency(bands[band].freq - IF_FREQ);  
 /****************************************************************************************
 *  begin to queue the audio from the audio library
 ****************************************************************************************/
    delay(100);
    Q_in_L.begin();
    Q_in_R.begin();    
} // END SETUP

int16_t *sp_L;
int16_t *sp_R;

void loop() {

 elapsedMicros usec = 0;
/**********************************************************************************
 *  Get samples from queue buffers
 **********************************************************************************/
    // we have to ensure that we have enough audio samples: we need
    // N_BLOCKS = fft_length / 2 / BUFFER_SIZE
    // FFT1024 point --> n_blocks = 1024 / 2 / 128 = 4 
    // when these buffers are available, read them in and perform
    // the FFT - cmplx-mult - iFFT
    //
    // are there at least N_BLOCKS buffers in each channel available ?
    if (Q_in_L.available() > N_BLOCKS && Q_in_R.available() > N_BLOCKS && audio_flag)
    {   
// get audio samples from the audio  buffers and convert them to float
    for (i = 0; i < N_BLOCKS; i++)
    {  
    sp_L = Q_in_L.readBuffer();
    sp_R = Q_in_R.readBuffer();

      // convert to float one buffer_size
     arm_q15_to_float (sp_L, &float_buffer_L[BUFFER_SIZE * i], BUFFER_SIZE); // convert int_buffer to float 32bit
     arm_q15_to_float (sp_R, &float_buffer_R[BUFFER_SIZE * i], BUFFER_SIZE); // convert int_buffer to float 32bit
     Q_in_L.freeBuffer();
     Q_in_R.freeBuffer();
    }

    // this is supposed to prevent overfilled queue buffers
    // rarely the Teensy audio queue gets a hickup
    // in that case this keeps the whole audio chain running smoothly 
    if (Q_in_L.available() > N_BLOCKS + 2 || Q_in_R.available() > N_BLOCKS + 2) 
    {
      Q_in_L.clear();
      Q_in_R.clear();
      n_clear ++; // just for debugging to check how often this occurs [about once in an hour of playing . . .]
    }
  
/**********************************************************************************
 *  Digital convolution
 **********************************************************************************/
//  basis for this was Lyons, R. (2011): Understanding Digital Processing.
//  "Fast FIR Filtering using the FFT", pages 688 - 694
//  numbers for the steps taken from that source
//  Method used here: overlap-and-save

// 4.) ONLY FOR the VERY FIRST FFT: fill first samples with zeros 
      if(first_block) // fill real & imaginaries with zeros for the first BLOCKSIZE samples
      {
        for(i = 0; i < BUFFER_SIZE * 2 * N_BLOCKS; i++)
        {
          FFT_buffer[i] = 0.0;
        }
        first_block = 0;
      }
      else
      
// HERE IT STARTS for all other instances
// 6 a.) fill FFT_buffer with last events audio samples
      for(i = 0; i < BUFFER_SIZE * N_BLOCKS; i++)
      {
        FFT_buffer[i * 2] = last_sample_buffer_L[i]; // real
        FFT_buffer[i * 2 + 1] = last_sample_buffer_R[i]; // imaginary
      }

    // copy recent samples to last_sample_buffer for next time!
      for(i = 0; i < BUFFER_SIZE * N_BLOCKS; i++)
      {
         last_sample_buffer_L [i] = float_buffer_L[i]; 
         last_sample_buffer_R [i] = float_buffer_R[i]; 
      }
    
//    arm_copy_f32(float_buffer_L, last_sample_buffer_L, BUFFER_SIZE * N_BLOCKS);
//    arm_copy_f32(float_buffer_R, last_sample_buffer_R, BUFFER_SIZE * N_BLOCKS);

// 6. b) now fill audio samples into FFT_buffer (left channel: re, right channel: im)
      for(i = 0; i < BUFFER_SIZE * N_BLOCKS; i++)
      {
        FFT_buffer[FFT_length + i * 2] = float_buffer_L[i]; // real
        FFT_buffer[FFT_length + i * 2 + 1] = float_buffer_R[i]; // imaginary
      }

// perform complex FFT
// calculation is performed in-place the FFT_buffer [re, im, re, im, re, im . . .]
      arm_cfft_f32(S, FFT_buffer, 0, 1);

// calculate 256 magnitude values from FFT_buffer and stuff into FFT_magn[]
// for spectrum display !
     calc_spectrum_mags(spectrum_zoom, LPF_spectrum);

// frequency shift & choice of FFT_bins for "demodulation"
    switch (demod_mode) 
    {
      case DEMOD_AM:
////////////////////////////////////////////////////////////////////////////////
// AM demodulation - shift everything FFT_length / 4 bins around IF 
// leave both: USB & LSB
////////////////////////////////////////////////////////////////////////////////
// shift USB part by FFT_shift
    for(i = 0; i < FFT_shift; i++)
    {
        FFT_buffer[i] = FFT_buffer[i + FFT_length * 2 - FFT_shift];
    }
    
// now shift LSB part by FFT_shift
    for(i = (FFT_length * 2) - FFT_shift - 2; i > FFT_length - FFT_shift;i--)
     {
        FFT_buffer[i + FFT_shift] = FFT_buffer[i]; // shift LSB part by FFT_shift bins
     }
     break;
///// END AM-demodulation, seems to be OK (at least sounds ok)     
////////////////////////////////////////////////////////////////////////////////
// USB DEMODULATION
////////////////////////////////////////////////////////////////////////////////
    case DEMOD_USB:
    case DEMOD_STEREO_USB:
    for(i = FFT_length / 2; i < FFT_length ; i++) // maybe this is not necessary? yes, it is!
     {
        FFT_buffer[i] = FFT_buffer [i - FFT_shift]; // shift USB, 2nd part, to the right
     }

    // frequency shift of the USB 1st part - but this is essential 
    for(i = FFT_length * 2 - FFT_shift; i < FFT_length * 2; i++)
    {
        FFT_buffer[i - FFT_length * 2 + FFT_shift] = FFT_buffer[i];
    }
    // fill LSB with zeros
    for(i = FFT_length; i < FFT_length *2; i++)
     {
        FFT_buffer[i] = 0.0; // fill rest with zero
     }
    break;
// END USB: audio seems to be OK 
////////////////////////////////////////////////////////////////////////////////    
    case DEMOD_LSB:
    case DEMOD_STEREO_LSB:
////////////////////////////////////////////////////////////////////////////////
// LSB demodulation
////////////////////////////////////////////////////////////////////////////////
    for(i = 0; i < FFT_length ; i++)
     {
        FFT_buffer[i] = 0.0; // fill USB with zero
     }
    // frequency shift of the LSB part 
    ////////////////////////////////////////////////////////////////////////////////
    for(i = (FFT_length * 2) - FFT_shift - 2; i > FFT_length - FFT_shift;i--)
     {
        FFT_buffer[i + FFT_shift] = FFT_buffer[i]; // shift LSB part by FFT_shift bins
     }
// fill USB with zeros
    for(i = FFT_length; i < FFT_length + FFT_shift; i++)
     {
        FFT_buffer[i] = 0.0; // fill rest with zero
     }
    break;
// END LSB: audio seems to be OK 
////////////////////////////////////////////////////////////////////////////////    
    } // END switch demod_mode
     
// complex multiply with filter mask
     arm_cmplx_mult_cmplx_f32 (FFT_buffer, FIR_filter_mask, iFFT_buffer, FFT_length);

// perform iFFT (in-place)
     arm_cfft_f32(iS, iFFT_buffer, 1, 1);

// our desired output is a combination of the real part (left channel) AND the imaginary part (right channel) of the second half of the FFT_buffer
// which one and how they are combined is dependent upon the demod_mode . . .

      if(demod_mode == DEMOD_AM)
      { // this AM demodulator with arm_cmplx_mag_f32 saves 1.23% of processor load at 96ksps compared to using arm_sqrt_f32
        arm_cmplx_mag_f32(&iFFT_buffer[FFT_length], float_buffer_R, FFT_length/2);
        // DC removal
        last_dc_level = fastdcblock_ff(float_buffer_R, float_buffer_L, FFT_length / 2, last_dc_level);
        // lowpass-filter the AM output to reduce high frequency noise produced by the envelope demodulator
        // see Whiteley 2011 for an explanation of this problem
        // 45.18% with IIR; 43.29% without IIR 
        arm_biquad_cascade_df1_f32 (&biquad_lowpass1, float_buffer_L, float_buffer_R, FFT_length / 2);
        arm_copy_f32(float_buffer_R, float_buffer_L, FFT_length/2);
      }
      else
      {
      for(i = 0; i < BUFFER_SIZE * N_BLOCKS; i++)
      {
        if(demod_mode == DEMOD_USB || demod_mode == DEMOD_LSB)
        {
            float_buffer_L[i] = iFFT_buffer[FFT_length + (i * 2)]; 
            // for SSB copy real part in both outputs
            float_buffer_R[i] = float_buffer_L[i];
        }
        else if(demod_mode == DEMOD_AM) // OLD AM demodulation routine
        {
            // envelope detection: AM = sqrt(I*I+Q*Q)
            // the standard sqrt-function takes too much time!
            // audiotmp = sqrt((iFFT_buffer[FFT_length + (i * 2)] * iFFT_buffer[FFT_length + (i * 2)]) + (iFFT_buffer[FFT_length + (i * 2) + 1] * iFFT_buffer[FFT_length + (i * 2) + 1])); 
            // this magnitude estimator works and saves about 2% processor load at 96ksps, but introduces annoying high pitch noise artefacts into the signal
//              audiotmp = alpha_beta_mag(iFFT_buffer[FFT_length + (i * 2)], iFFT_buffer[FFT_length + (i * 2) + 1]);
            arm_sqrt_f32((iFFT_buffer[FFT_length + (i * 2)] * iFFT_buffer[FFT_length + (i * 2)]) + (iFFT_buffer[FFT_length + (i * 2) + 1] * iFFT_buffer[FFT_length + (i * 2) + 1]), &audiotmp);
            // DC removal filter ----------------------- 
            w = audiotmp + wold * 0.9999f; // yes, I want a superb bass response ;-) 
            float_buffer_L[i] = w - wold; 
            wold = w; 
            // -----------------------------------------
            float_buffer_R[i] = float_buffer_L[i];
        }
        else if(demod_mode == DEMOD_STEREO_LSB || demod_mode == DEMOD_STEREO_USB) // creates a pseudo-stereo effect
            // could be good for copying faint CW signals
        {
            float_buffer_L[i] = iFFT_buffer[FFT_length + (i * 2)]; 
            float_buffer_R[i] = iFFT_buffer[FFT_length + (i * 2) + 1];
        }
      } // else AM
      }

    for (i = 0; i < N_BLOCKS; i++)
    {
      sp_L = Q_out_L.getBuffer();
      sp_R = Q_out_R.getBuffer();
      arm_float_to_q15 (&float_buffer_L[BUFFER_SIZE * i], sp_L, BUFFER_SIZE); 
      arm_float_to_q15 (&float_buffer_R[BUFFER_SIZE * i], sp_R, BUFFER_SIZE); 
      Q_out_L.playBuffer(); // play it !
      Q_out_R.playBuffer(); // play it !
    }  

/**********************************************************************************
 *  PRINT ROUTINE FOR ELAPSED MICROSECONDS
 **********************************************************************************/
 
      sum = sum + usec;
      idx_t++;
      if (idx_t > 100) {
          tft.fillRect(260,5,60,20,ILI9341_BLACK);   
          tft.setCursor(260, 5);
          tft.setTextColor(ILI9341_GREEN);
          mean = sum / idx_t;
          tft.print (mean/29.00/N_BLOCKS * SR[SAMPLE_RATE].rate / AUDIO_SAMPLE_RATE_EXACT);tft.print("%");
          Serial.print (mean);
          Serial.print (" microsec for ");
          Serial.print (N_BLOCKS);
          Serial.print ("  stereo blocks    ");
          Serial.println();
          Serial.print (" n_clear    ");
          Serial.println(n_clear);
          idx_t = 0;
          sum = 0;
          tft.setTextColor(ILI9341_WHITE);
      }

     }
/**********************************************************************************
 *  PRINT ROUTINE FOR AUDIO LIBRARY PROCESSOR AND MEMORY USAGE
 **********************************************************************************/
          if (five_sec.check() == 1)
    {
      Serial.print("Proc = ");
      Serial.print(AudioProcessorUsage());
      Serial.print(" (");    
      Serial.print(AudioProcessorUsageMax());
      Serial.print("),  Mem = ");
      Serial.print(AudioMemoryUsage());
      Serial.print(" (");    
      Serial.print(AudioMemoryUsageMax());
      Serial.println(")");
      AudioProcessorUsageMaxReset();
      AudioMemoryUsageMaxReset();
    }
    show_spectrum();
    filter_bandwidth();
    encoders();
    buttons();
}

void filter_bandwidth() {
    LP_F_help = analogRead(15) * 10.0;
    LP_F_help = (int)((float)LP_F_help * 0.3 + 0.7 * (float)LP_Fpass_old);
    LP_F_help = (int)(LP_F_help/100 * 100);
    LP_F_help += 100;
    if(LP_F_help != LP_Fpass_old)
    { //audio_flag_counter = 1000;

    calc_FIR_coeffs (FIR_Coef, 2049, (float32_t)LP_F_help, LP_Astop, 0, 0.0, SR[SAMPLE_RATE].rate);
    init_filter_mask();

    // also adjust IIR AM filter
    set_IIR_coeffs ((float32_t)LP_F_help / 2.0, 1.3, (float32_t)SR[SAMPLE_RATE].rate, 0); // 1st stage
    for(i = 0; i < 5; i++)
    {
        biquad_lowpass1_coeffs[i] = coefficient_set[i];
    }
    
    show_bandwidth (demod_mode, LP_F_help);
    }      
    LP_Fpass_old = LP_F_help; 
}

void calc_FIR_coeffs (float * coeffs, int numCoeffs, float32_t fc, float32_t Astop, int type, float dfc, float Fsamprate)
    // pointer to coefficients variable, no. of coefficients to calculate, frequency where it happens, stopband attenuation in dB, 
    // filter type, half-filter bandwidth (only for bandpass and notch) 
 {  // modified by WMXZ and DD4WH after
    // Wheatley, M. (2011): CuteSDR Technical Manual. www.metronix.com, pages 118 - 120, FIR with Kaiser-Bessel Window
    // assess required number of coefficients by
    //     numCoeffs = (Astop - 8.0) / (2.285 * TPI * normFtrans);
    // selecting high-pass, numCoeffs is forced to an even number for better frequency response

     int ii,jj;
     float32_t Beta;
     float32_t izb;
     float fcf;
     int nc;
     fc = fc / Fsamprate;
     dfc = dfc / Fsamprate;
     // calculate Kaiser-Bessel window shape factor beta from stop-band attenuation
     if (Astop < 20.96)
       Beta = 0.0;
     else if (Astop >= 50.0)
       Beta = 0.1102 * (Astop - 8.71);
     else
       Beta = 0.5842 * powf((Astop - 20.96), 0.4) + 0.07886 * (Astop - 20.96);

     izb = Izero (Beta);
     if(type == 0) // low pass filter
     {  fcf = fc;
      nc =  numCoeffs;
     }
     else if(type == 1) // high-pass filter
     {  fcf = -fc;
      nc =  2*(numCoeffs/2);
     }
     else if ((type == 2) || (type==3)) // band-pass filter
     {
       fcf = dfc;
       nc =  2*(numCoeffs/2); // maybe not needed
     }
     else if (type==4)  // Hilbert transform
   {
         nc =  2*(numCoeffs/2);
       // clear coefficients
       for(ii=0; ii< 2*(nc-1); ii++) coeffs[ii]=0;
       // set real delay
       coeffs[nc]=1;

       // set imaginary Hilbert coefficients
       for(ii=1; ii< (nc+1); ii+=2)
       {
         if(2*ii==nc) continue;
       float x =(float)(2*ii - nc)/(float)nc;
       float w = Izero(Beta*sqrtf(1.0f - x*x))/izb; // Kaiser window
       coeffs[2*ii+1] = 1.0f/(PIH*(float)(ii-nc/2)) * w ;
       }
       return;
   }

     for(ii= - nc, jj=0; ii< nc; ii+=2,jj++)
     {
       float x =(float)ii/(float)nc;
       float w = Izero(Beta*sqrtf(1.0f - x*x))/izb; // Kaiser window
       coeffs[jj] = fcf * m_sinc(ii,fcf) * w;
     }

     if(type==1)
     {
       coeffs[nc/2] += 1;
     }
     else if (type==2)
     {
         for(jj=0; jj< nc+1; jj++) coeffs[jj] *= 2.0f*cosf(PIH*(2*jj-nc)*fc);
     }
     else if (type==3)
     {
         for(jj=0; jj< nc+1; jj++) coeffs[jj] *= -2.0f*cosf(PIH*(2*jj-nc)*fc);
       coeffs[nc/2] += 1;
     }

} // END calc_FIR_coeffs

float m_sinc(int m, float fc)
{  // fc is f_cut/(Fsamp/2)
  // m is between -M and M step 2
  //
  float x = m*PIH;
  if(m == 0)
    return 1.0f;
  else
    return sinf(x*fc)/(fc*x);
}
/*
void calc_FIR_lowpass_coeffs (float32_t Scale, float32_t Astop, float32_t Fpass, float32_t Fstop, float32_t Fsamprate) 
{ // Wheatley, M. (2011): CuteSDR Technical Manual. www.metronix.com, pages 118 - 120, FIR with Kaiser-Bessel Window
    int n;
    float32_t Beta;
    float32_t izb;
    // normalized F parameters
    float32_t normFpass = Fpass / Fsamprate;
    float32_t normFstop = Fstop / Fsamprate;
    float32_t normFcut = (normFstop + normFpass) / 2.0; // lowpass filter 6dB cutoff
    // calculate Kaiser-Bessel window shape factor beta from stopband attenuation
    if (Astop < 20.96)
    {
      Beta = 0.0;    
    }
    else if (Astop >= 50.0)
    {
      Beta = 0.1102 * (Astop - 8.71);
    }
    else
    {
      Beta = 0.5842 * powf((Astop - 20.96), 0.4) + 0.07886 * (Astop - 20.96);
    }
    // estimate number of taps
    m_NumTaps = (int32_t)((Astop - 8.0) / (2.285 * K_2PI * (normFstop - normFpass)) + 1.0);
    if (m_NumTaps > MAX_NUMCOEF) 
    { 
      m_NumTaps = MAX_NUMCOEF;
    }
    if (m_NumTaps < 3)
    {
      m_NumTaps = 3;
    }
    float32_t fCenter = 0.5 * (float32_t) (m_NumTaps - 1);
    izb = Izero (Beta);
    for (n = 0; n < m_NumTaps; n++)
    {
      float32_t x = (float32_t) n - fCenter;
      float32_t c;
      // create ideal Sinc() LP filter with normFcut
        if ( abs((float)n - fCenter) < 0.01)
      { // deal with odd size filter singularity where sin(0) / 0 == 1
        c = 2.0 * normFcut;
//        Serial.println("Hello, here at odd FIR filter size");
      }
      else
      {
        c = (float32_t) sinf(K_2PI * x * normFcut) / (K_PI * x);
//        c = (float32_t) sinf(K_PI * x * normFcut) / (K_PI * x);
      }
      x = ((float32_t) n - ((float32_t) m_NumTaps - 1.0) / 2.0) / (((float32_t) m_NumTaps - 1.0) / 2.0);
      FIR_Coef[n] = Scale * c * Izero (Beta * sqrt(1 - (x * x) )) / izb;
    }
} // END calc_lowpass_coeffs
*/
float32_t Izero (float32_t x) 
{
    float32_t x2 = x / 2.0;
    float32_t summe = 1.0;
    float32_t ds = 1.0;
    float32_t di = 1.0;
    float32_t errorlimit = 1e-9;
    float32_t tmp;
    do
    {
        tmp = x2 / di;
        tmp *= tmp;
        ds *= tmp;
        summe += ds;
        di += 1.0; 
    }   while (ds >= errorlimit * summe);
    return (summe);
}  // END Izero

// set samplerate code by Frank Boesing 
void setI2SFreq(int freq) {
  typedef struct {
    uint8_t mult;
    uint16_t div;
  } tmclk;

  const int numfreqs = 14;
  const int samplefreqs[numfreqs] = { 8000, 11025, 16000, 22050, 32000, 44100, (int)44117.64706 , 48000, 88200, (int)44117.64706 * 2, 96000, 176400, (int)44117.64706 * 4, 192000};

#if (F_PLL==16000000)
  const tmclk clkArr[numfreqs] = {{16, 125}, {148, 839}, {32, 125}, {145, 411}, {64, 125}, {151, 214}, {12, 17}, {96, 125}, {151, 107}, {24, 17}, {192, 125}, {127, 45}, {48, 17}, {255, 83} };
#elif (F_PLL==72000000)
  const tmclk clkArr[numfreqs] = {{32, 1125}, {49, 1250}, {64, 1125}, {49, 625}, {128, 1125}, {98, 625}, {8, 51}, {64, 375}, {196, 625}, {16, 51}, {128, 375}, {249, 397}, {32, 51}, {185, 271} };
#elif (F_PLL==96000000)
  const tmclk clkArr[numfreqs] = {{8, 375}, {73, 2483}, {16, 375}, {147, 2500}, {32, 375}, {147, 1250}, {2, 17}, {16, 125}, {147, 625}, {4, 17}, {32, 125}, {151, 321}, {8, 17}, {64, 125} };
#elif (F_PLL==120000000)
  const tmclk clkArr[numfreqs] = {{32, 1875}, {89, 3784}, {64, 1875}, {147, 3125}, {128, 1875}, {205, 2179}, {8, 85}, {64, 625}, {89, 473}, {16, 85}, {128, 625}, {178, 473}, {32, 85}, {145, 354} };
#elif (F_PLL==144000000)
  const tmclk clkArr[numfreqs] = {{16, 1125}, {49, 2500}, {32, 1125}, {49, 1250}, {64, 1125}, {49, 625}, {4, 51}, {32, 375}, {98, 625}, {8, 51}, {64, 375}, {196, 625}, {16, 51}, {128, 375} };
#elif (F_PLL==168000000)
  const tmclk clkArr[numfreqs] = {{32, 2625}, {21, 1250}, {64, 2625}, {21, 625}, {128, 2625}, {42, 625}, {8, 119}, {64, 875}, {84, 625}, {16, 119}, {128, 875}, {168, 625}, {32, 119}, {189, 646} };
#elif (F_PLL==180000000)
  const tmclk clkArr[numfreqs] = {{46, 4043}, {49, 3125}, {73, 3208}, {98, 3125}, {183, 4021}, {196, 3125}, {16, 255}, {128, 1875}, {107, 853}, {32, 255}, {219, 1604}, {214, 853}, {64, 255}, {219, 802} };
#elif (F_PLL==192000000)
  const tmclk clkArr[numfreqs] = {{4, 375}, {37, 2517}, {8, 375}, {73, 2483}, {16, 375}, {147, 2500}, {1, 17}, {8, 125}, {147, 1250}, {2, 17}, {16, 125}, {147, 625}, {4, 17}, {32, 125} };
#elif (F_PLL==216000000)
  const tmclk clkArr[numfreqs] = {{32, 3375}, {49, 3750}, {64, 3375}, {49, 1875}, {128, 3375}, {98, 1875}, {8, 153}, {64, 1125}, {196, 1875}, {16, 153}, {128, 1125}, {226, 1081}, {32, 153}, {147, 646} };
#elif (F_PLL==240000000)
  const tmclk clkArr[numfreqs] = {{16, 1875}, {29, 2466}, {32, 1875}, {89, 3784}, {64, 1875}, {147, 3125}, {4, 85}, {32, 625}, {205, 2179}, {8, 85}, {64, 625}, {89, 473}, {16, 85}, {128, 625} };
#endif

  for (int f = 0; f < numfreqs; f++) {
    if ( freq == samplefreqs[f] ) {
      while (I2S0_MCR & I2S_MCR_DUF) ;
      I2S0_MDR = I2S_MDR_FRACT((clkArr[f].mult - 1)) | I2S_MDR_DIVIDE((clkArr[f].div - 1));
      return;
    }
  }
}

void init_filter_mask() {
   /****************************************************************************************
 *  Calculate the FFT of the FIR filter coefficients once to produce the FIR filter mask
 ****************************************************************************************/
// the FIR has exactly m_NumTaps and a maximum of (FFT_length / 2) + 1 taps = coefficients, so we have to add (FFT_length / 2) -1 zeros before the FFT
// in order to produce a FFT_length point input buffer for the FFT 
    // copy coefficients into real values of first part of buffer, rest is zero

    for (i = 0; i < (FFT_length / 2) + 1; i++)
    {
        FIR_filter_mask[i * 2] = FIR_Coef [i];
        FIR_filter_mask[i * 2 + 1] = 0.0; 
    }
    
    for (i = FFT_length; i < FFT_length * 2; i++)
    {
        FIR_filter_mask[i] = 0.0;
    }

    
// FFT of FIR_filter_mask
// perform FFT (in-place), needs only to be done once (or every time the filter coeffs change)
    arm_cfft_f32(maskS, FIR_filter_mask, 0, 1);    
    
///////////////////////////////////////////////////////////////77
// PASS-THRU only for TESTING
/////////////////////////////////////////////////////////////77
/*
// hmmm, unclear, whether [1,1] or [1,0] or [0,1] are pass-thru filter-mask-multipliers??
// empirically, [1,0] sounds most natural = pass-thru
  for(i = 0; i < FFT_length * 2; i+=2)
  {
        FIR_filter_mask [i] = 1.0; // real
        FIR_filter_mask [i + 1] = 0.0; // imaginary
  }
*/
}

void calc_spectrum_mags(uint32_t zoom, float32_t LPFcoeff) {
      float32_t help, help2;
      LPFcoeff = LPFcoeff * AUDIO_SAMPLE_RATE_EXACT / SR[SAMPLE_RATE].rate;
      if(LPFcoeff > 1.0) LPFcoeff = 1.0;
      for(i = 0; i < 256; i++)
      {
          pixelold[i] = pixelnew[i];
      }
/*      // this saves absolutely NO processor power at 96ksps in comparison to arm_cmplx_mag
      for(i = 0; i < FFT_length; i++)
      {
          FFT_magn[i] = alpha_beta_mag(FFT_buffer[(i * 2)], FFT_buffer[(i*2)+1]);
      }
*/      
      arm_cmplx_mag_f32(FFT_buffer, FFT_magn, FFT_length);  // calculates sqrt(I*I + Q*Q) for each frequency bin of the FFT
      
      ////////////////////////////////////////////////////////////////////////
      // now calculate average of the bin values according to spectrum zoom
      // this assumes FFT_shift of 1024, ie. 5515Hz with an FFT_length of 4096
      ////////////////////////////////////////////////////////////////////////
      
      // TODO: adapt to different FFT lengths
      for(i = 0; i < FFT_length / 2; i+=zoom)
      {
              arm_mean_f32(&FFT_magn[i], zoom, &FFT_spec[i / zoom + 128]);
      }
      for(i = FFT_length / 2; i < FFT_length; i+=zoom)
      {
              arm_mean_f32(&FFT_magn[i], zoom, &FFT_spec[i / zoom - 128]);
      }


      // low pass filtering of the spectrum pixels to smooth/slow down spectrum in the time domain
      // ynew = LPCoeff * x + (1-LPCoeff) * yprevious; 
      for(int16_t x = 0; x < 256; x++)
      {
           help = LPFcoeff * FFT_spec[x] + (1.0 - LPFcoeff) * FFT_spec_old[x];
           FFT_spec[x] = help;
           FFT_spec_old[x] = FFT_spec[x];
              // insert display offset, AGC etc. here
              // a single log10 here needs another 22% of processor use on a Teensy 3.6 (96k sample rate)!
//           help = 50.0 * log10(help+1.0); 
           arm_sqrt_f32(help, &help2);          
           pixelnew[x] = (int16_t) (help2 * 25);
      }
      
} // end calc_spectrum_mags

void show_spectrum() {
      int16_t y_old, y_new, y1_new, y1_old;
      int16_t y1_old_minus = 0;
      int16_t y1_new_minus = 0;

      // Draw spectrum display
      for (int16_t x = 0; x < 254; x++) 
      {
                if ((x > 1) && (x < 255)) 
                // moving window - weighted average of 5 points of the spectrum to smooth spectrum in the frequency domain
                // weights:  x: 50% , x-1/x+1: 36%, x+2/x-2: 14% 
                {    
                  y_new = pixelnew[x] * 0.5 + pixelnew[x - 1] * 0.18 + pixelnew[x + 1] * 0.18 + pixelnew[x - 2] * 0.07 + pixelnew[x + 2] * 0.07;
                  y_old = pixelold[x] * 0.5 + pixelold[x - 1] * 0.18 + pixelold[x + 1] * 0.18 + pixelold[x - 2] * 0.07 + pixelold[x + 2] * 0.07;
                }
                else 
                {
                  y_new = pixelnew[x];
                  y_old = pixelold[x];
                 }
               if(y_old > (spectrum_height - 7))
               {
                  y_old = (spectrum_height - 7);
               }

               if(y_new > (spectrum_height - 7))
               {
                  y_new = (spectrum_height - 7);
               }
               y1_old  = (spectrum_y + spectrum_height - 1) - y_old;
               y1_new  = (spectrum_y + spectrum_height - 1) - y_new; 

               if(x == 0)
               {
                   y1_old_minus = y1_old;
                   y1_new_minus = y1_new;
               }
               if(x == 254)
               {
                   y1_old_minus = y1_old;
                   y1_new_minus = y1_new;
               }

             if(x != spectrum_pos_centre_f) 
             {
          // DELETE OLD LINE/POINT
              if(y1_old - y1_old_minus > 1) 
               { // plot line upwards
                tft.drawFastVLine(x + spectrum_x, y1_old_minus + 1, y1_old - y1_old_minus,ILI9341_BLACK);
               }
              else if (y1_old - y1_old_minus < -1) 
               { // plot line downwards
                tft.drawFastVLine(x + spectrum_x, y1_old, y1_old_minus - y1_old,ILI9341_BLACK);
               }
              else
               {
                  tft.drawPixel(x + spectrum_x, y1_old, ILI9341_BLACK); // delete old pixel
               }

          // DRAW NEW LINE/POINT
              if(y1_new - y1_new_minus > 1) 
               { // plot line upwards
                tft.drawFastVLine(x + spectrum_x, y1_new_minus + 1, y1_new - y1_new_minus,ILI9341_WHITE);
               }
              else if (y1_new - y1_new_minus < -1) 
               { // plot line downwards
                tft.drawFastVLine(x + spectrum_x, y1_new, y1_new_minus - y1_new,ILI9341_WHITE);
               }
               else
               {
                  tft.drawPixel(x + spectrum_x, y1_new, ILI9341_WHITE); // write new pixel
               }

            y1_new_minus = y1_new;
            y1_old_minus = y1_old;

        }
      } // end for loop

} // END show_spectrum


void show_bandwidth (int M, uint32_t filter_f)  
{  // M = demod_mode, FU & FL upper & lower frequency
  // this routine prints the frequency bars under the spectrum display
  // and displays the bandwidth bar indicating demodulation bandwidth
   float32_t pixel_per_khz = 256.0 / SR[SAMPLE_RATE].rate * 500.0;
   float32_t leU, leL;
   char string[4];
   int pos_y = spectrum_y + spectrum_height + 2; 
   tft.drawFastHLine(spectrum_x, pos_y, 256, ILI9341_BLACK); // erase old indicator
   tft.drawFastHLine(spectrum_x, pos_y + 1, 256, ILI9341_BLACK); // erase old indicator 
   tft.drawFastHLine(spectrum_x, pos_y + 2, 256, ILI9341_BLACK); // erase old indicator
   tft.drawFastHLine(spectrum_x, pos_y + 3, 256, ILI9341_BLACK); // erase old indicator

    switch(M) {
      case DEMOD_AM:
          leU = filter_f / 1000.0 * pixel_per_khz;
          leL = filter_f / 1000.0 * pixel_per_khz;
          break;
      case DEMOD_LSB:
          leU = 0.0;
          leL = filter_f / 1000.0 * pixel_per_khz;
          break;
      case DEMOD_USB:
          leU = filter_f / 1000.0 * pixel_per_khz;
          leL = 0.0;
    }
      if(leL > spectrum_pos_centre_f) leL = spectrum_pos_centre_f;
     
      // draw upper sideband indicator
      tft.drawFastHLine(spectrum_pos_centre_f + spectrum_x + 1, pos_y, leU, ILI9341_YELLOW);
      tft.drawFastHLine(spectrum_pos_centre_f + spectrum_x + 1, pos_y + 1, leU, ILI9341_YELLOW);
      tft.drawFastHLine(spectrum_pos_centre_f + spectrum_x + 1, pos_y + 2, leU, ILI9341_YELLOW);
      tft.drawFastHLine(spectrum_pos_centre_f + spectrum_x + 1, pos_y + 3, leU, ILI9341_YELLOW);
      // draw lower sideband indicator   
      tft.drawFastHLine(spectrum_pos_centre_f  + spectrum_x - leL + 1, pos_y, leL + 1, ILI9341_YELLOW);
      tft.drawFastHLine(spectrum_pos_centre_f  + spectrum_x - leL + 1, pos_y + 1, leL + 1, ILI9341_YELLOW);
      tft.drawFastHLine(spectrum_pos_centre_f  + spectrum_x - leL + 1, pos_y + 2, leL + 1, ILI9341_YELLOW);
      tft.drawFastHLine(spectrum_pos_centre_f  + spectrum_x - leL + 1, pos_y + 3, leL + 1, ILI9341_YELLOW);
      //print bandwidth !
          tft.fillRect(10,25,90,20,ILI9341_BLACK);   
          tft.setCursor(10, 25);
          tft.setFont(Arial_10);
          tft.print(DEMOD[demod_mode].text);
          sprintf(string,"%02.1f kHz", (float)(filter_f/1000.0));//kHz);
          tft.setCursor(70, 25);
          tft.print(string);
          tft.setCursor(140, 25);
          tft.print("   SR:  ");
          tft.print(SR[SAMPLE_RATE].text);
          tft.setTextColor(ILI9341_WHITE); // set text color to white for other print routines not to get confused ;-)
}  

void prepare_spectrum_display() {
    uint16_t base_y = spectrum_y + spectrum_height + 4;
    uint16_t b_x = spectrum_x + SR[SAMPLE_RATE].x_offset;
    float32_t x_f = SR[SAMPLE_RATE].x_factor;
    tft.fillRect(0,base_y,320,240 - base_y,ILI9341_BLACK);
    tft.drawRect(spectrum_x - 2, spectrum_y + 2, 257, spectrum_height, ILI9341_MAROON); 
    // receive freq indicator line
    tft.drawFastVLine(spectrum_x + spectrum_pos_centre_f, spectrum_y + spectrum_height - 18, 20, ILI9341_GREEN); 

    // vertical lines
    tft.drawFastVLine(b_x - 4, base_y + 1, 10, ILI9341_YELLOW);  
    tft.drawFastVLine(b_x - 3, base_y + 1, 10, ILI9341_YELLOW);  
    tft.drawFastVLine( x_f * 2.0 + b_x,  base_y + 1, 10, ILI9341_YELLOW);  
    tft.drawFastVLine( x_f * 2.0 + 1.0 + b_x,  base_y + 1, 10, ILI9341_YELLOW);  
    if(x_f * 3.0 + b_x < 256+b_x) {
    tft.drawFastVLine( x_f * 3.0 + b_x,  base_y + 1, 10, ILI9341_YELLOW);  
    tft.drawFastVLine( x_f * 3.0 + 1.0 + b_x,  base_y + 1, 10, ILI9341_YELLOW);  
    }
    if(x_f * 4.0 + b_x < 256+b_x) {
    tft.drawFastVLine( x_f * 4.0 + b_x,  base_y + 1, 10, ILI9341_YELLOW);  
    tft.drawFastVLine( x_f * 4.0 + 1.0 + b_x,  base_y + 1, 10, ILI9341_YELLOW);  
    }
    if(x_f * 5.0 + b_x < 256+b_x) {
    tft.drawFastVLine( x_f * 5.0 + b_x,  base_y + 1, 10, ILI9341_YELLOW);  
    tft.drawFastVLine( x_f * 5.0 + 1.0 + b_x,  base_y + 1, 10, ILI9341_YELLOW);  
    }
    tft.drawFastVLine( x_f * 0.5 + b_x,  base_y + 1, 6, ILI9341_YELLOW);  
    tft.drawFastVLine( x_f * 1.5 + b_x,  base_y + 1, 6, ILI9341_YELLOW);  
    tft.drawFastVLine( x_f * 2.5 + b_x,  base_y + 1, 6, ILI9341_YELLOW);  
    if(x_f * 3.5 + b_x < 256+b_x) {
    tft.drawFastVLine( x_f * 3.5 + b_x,  base_y + 1, 6, ILI9341_YELLOW);  
    }
    if(x_f * 4.5 + b_x < 256+b_x) {
        tft.drawFastVLine( x_f * 4.5 + b_x,  base_y + 1, 6, ILI9341_YELLOW);  
    }
    // text
    tft.setTextColor(ILI9341_WHITE);
    tft.setFont(Arial_8);
    int text_y_offset = 15;
    int text_x_offset = - 5;
    // zero
    tft.setCursor (b_x + text_x_offset - 1, base_y + text_y_offset);
    tft.print(SR[SAMPLE_RATE].f1);
    tft.setCursor (b_x + x_f * 2 + text_x_offset, base_y + text_y_offset);
    tft.print(SR[SAMPLE_RATE].f2);
    tft.setCursor (b_x + x_f *3 + text_x_offset, base_y + text_y_offset);
    tft.print(SR[SAMPLE_RATE].f3);
    tft.setCursor (b_x + x_f *4 + text_x_offset, base_y + text_y_offset);
    tft.print(SR[SAMPLE_RATE].f4);
//    tft.setCursor (b_x + text_x_offset + 256, base_y + text_y_offset);
    tft.print(" kHz");

//    tft.setFont(Arial_14);
} // END prepare_spectrum_display


float32_t alpha_beta_mag(float32_t  inphase, // (c) András Retzler
                      float32_t  quadrature) // taken from libcsdr: https://github.com/simonyiszk/csdr
{
  // Min RMS Err      0.947543636291 0.392485425092
  // Min Peak Err     0.960433870103 0.397824734759
  // Min RMS w/ Avg=0 0.948059448969 0.392699081699
  const float32_t alpha = 0.960433870103; // 1.0; //0.947543636291;
  const float32_t beta =  0.397824734759;
   /* magnitude ~= alpha * max(|I|, |Q|) + beta * min(|I|, |Q|) */
   float32_t abs_inphase = fabs(inphase);
   float32_t abs_quadrature = fabs(quadrature);
   if (abs_inphase > abs_quadrature) {
      return alpha * abs_inphase + beta * abs_quadrature;
   } else {
      return alpha * abs_quadrature + beta * abs_inphase;
   }
}
/*
void amdemod_estimator_cf(complexf* input, float *output, int input_size, float alpha, float beta)
{ //  (c) András Retzler
  //  taken from libcsdr: https://github.com/simonyiszk/csdr
  //concept is explained here:
  //http://www.dspguru.com/dsp/tricks/magnitude-estimator

  //default: optimize for min RMS error
  if(alpha==0)
  {
    alpha=0.947543636291;
    beta=0.392485425092;
  }

  //@amdemod_estimator
  for (int i=0; i<input_size; i++)
  {
    float abs_i=iof(input,i);
    if(abs_i<0) abs_i=-abs_i;
    float abs_q=qof(input,i);
    if(abs_q<0) abs_q=-abs_q;
    float max_iq=abs_i;
    if(abs_q>max_iq) max_iq=abs_q;
    float min_iq=abs_i;
    if(abs_q<min_iq) min_iq=abs_q;

    output[i]=alpha*max_iq+beta*min_iq;
  }
}

*/

float32_t fastdcblock_ff(float32_t* input, float32_t* output, int input_size, float32_t last_dc_level)
{ //  (c) András Retzler
  //  taken from libcsdr: https://github.com/simonyiszk/csdr
  //this DC block filter does moving average block-by-block.
  //this is the most computationally efficient
  //input and output buffer is allowed to be the same
  //http://www.digitalsignallabs.com/dcblock.pdf
  float32_t avg=0.0;
  for(int i=0;i<input_size;i++) //@fastdcblock_ff: calculate block average
  {
    avg+=input[i];
  }
  avg/=input_size;

  float32_t avgdiff=avg-last_dc_level;
  //DC removal level will change lineraly from last_dc_level to avg.
  for(int i=0;i<input_size;i++) //@fastdcblock_ff: remove DC component
  {
    float32_t dc_removal_level=last_dc_level+avgdiff*((float32_t)i/input_size);
    output[i]=input[i]-dc_removal_level;
  }
  return avg;
}

void set_IIR_coeffs (float32_t f0, float32_t Q, float32_t sample_rate, uint8_t filter_type)
{      
 //     set_IIR_coeffs ((float32_t)2400.0, 1.3, (float32_t)SR[SAMPLE_RATE].rate, 0);
     /*+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
     * Cascaded biquad (notch, peak, lowShelf, highShelf) [DD4WH, april 2016]
     ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/
    // DSP Audio-EQ-cookbook for generating the coeffs of the filters on the fly
    // www.musicdsp.org/files/Audio-EQ-Cookbook.txt  [by Robert Bristow-Johnson]
    //
    // the ARM algorithm assumes the biquad form
    // y[n] = b0 * x[n] + b1 * x[n-1] + b2 * x[n-2] + a1 * y[n-1] + a2 * y[n-2]
    //
    // However, the cookbook formulae by Robert Bristow-Johnson AND the Iowa Hills IIR Filter designer
    // use this formula:
    //
    // y[n] = b0 * x[n] + b1 * x[n-1] + b2 * x[n-2] - a1 * y[n-1] - a2 * y[n-2]
    //
    // Therefore, we have to use negated a1 and a2 for use with the ARM function

    float32_t w0 = f0 * (TPI / sample_rate);
    float32_t sinW0 = sinf(w0);
    float32_t alpha = sinW0 / (Q * 2.0);
    float32_t cosW0 = cosf(w0);
    float32_t scale = 1.0 / (1.0 + alpha);

    
    /* b0 */ coefficient_set[0] = ((1.0 - cosW0) / 2.0) * scale;
    /* b1 */ coefficient_set[1] = (1.0 - cosW0) * scale;
    /* b2 */ coefficient_set[2] = coefficient_set[0];
    /* a1 */ coefficient_set[3] = (2.0 * cosW0) * scale; // negated
/* a2 */     coefficient_set[4] = (-1.0 + alpha) * scale; // negated
   
}

int ExtractDigit(long int n, int k) {
        switch (k) {
              case 0: return n%10;
              case 1: return n/10%10;
              case 2: return n/100%10;
              case 3: return n/1000%10;
              case 4: return n/10000%10;
              case 5: return n/100000%10;
              case 6: return n/1000000%10;
              case 7: return n/10000000%10;
              case 8: return n/100000000%10;
        }
}



// show frequency
void show_frequency(long int freq) { 
//    tft.setTextSize(2);
    tft.setFont(Arial_18);
    tft.setTextColor(ILI9341_WHITE);
    uint8_t zaehler;
    uint8_t digits[10];
    zaehler = 8;

          while (zaehler--) {
              digits[zaehler] = ExtractDigit (freq, zaehler);
//              Serial.print(digits[zaehler]);
//              Serial.print(".");
// 7: 10Mhz, 6: 1Mhz, 5: 100khz, 4: 10khz, 3: 1khz, 2: 100Hz, 1: 10Hz, 0: 1Hz
        }
            //  Serial.print("xxxxxxxxxxxxx");

    zaehler = 8;
        while (zaehler--) { // counts from 7 to 0
              if (zaehler < 6) sch = 9; // (khz)
              if (zaehler < 3) sch = 18; // (Hz)
          if (digits[zaehler] != digits_old[zaehler] || !freq_flag) { // digit has changed (or frequency is displayed for the first time after power on)
              if (zaehler == 7) {
                     sch = 0;
                     tft.setCursor(pos_x_frequency + font_width * (8-zaehler) + sch,pos_y_frequency); // set print position
                     tft.fillRect(pos_x_frequency + font_width * (8-zaehler) + sch,pos_y_frequency, font_width,18,ILI9341_BLACK); // delete old digit
                     if (digits[7] != 0) tft.print(digits[zaehler]); // write new digit in white
              }
              if (zaehler == 6) {
                            sch = 0;
                            tft.setCursor(pos_x_frequency + font_width * (8-zaehler) + sch,pos_y_frequency); // set print position
                            tft.fillRect(pos_x_frequency + font_width * (8-zaehler) + sch,pos_y_frequency, font_width,18,ILI9341_BLACK); // delete old digit
                            if (digits[6]!=0 || digits[7] != 0) tft.print(digits[zaehler]); // write new digit in white
              }
               if (zaehler == 5) {
                            sch = 9;
                            tft.setCursor(pos_x_frequency + font_width * (8-zaehler) + sch,pos_y_frequency); // set print position
                            tft.fillRect(pos_x_frequency + font_width * (8-zaehler) + sch,pos_y_frequency, font_width,18,ILI9341_BLACK); // delete old digit
                            if (digits[5] != 0 || digits[6]!=0 || digits[7] != 0) tft.print(digits[zaehler]); // write new digit in white
              }
              
              if (zaehler < 5) { 
              // print the digit
              tft.setCursor(pos_x_frequency + font_width * (8-zaehler) + sch,pos_y_frequency); // set print position
              tft.fillRect(pos_x_frequency + font_width * (8-zaehler) + sch,pos_y_frequency, font_width,18,ILI9341_BLACK); // delete old digit
              tft.print(digits[zaehler]); // write new digit in white
              }
              digits_old[zaehler] = digits[zaehler]; 
        }
        }
  
      tft.setFont(Arial_11);
      if (digits[7] == 0 && digits[6] == 0)
              tft.fillRect(pos_x_frequency + font_width * 3 ,pos_y_frequency + 15, 3, 3, ILI9341_BLACK);
      else    tft.fillRect(pos_x_frequency + font_width * 3 ,pos_y_frequency + 15, 3, 3, ILI9341_YELLOW);
      tft.fillRect(pos_x_frequency + font_width * 7 - 6, pos_y_frequency + 15, 3, 3, ILI9341_YELLOW);
      if (!freq_flag) {
            tft.setCursor(pos_x_frequency + font_width * 9 + 21,pos_y_frequency + 7); // set print position
            tft.setTextColor(ILI9341_GREEN);
            tft.print("Hz");
      }
      freq_flag = 1;
      tft.setTextColor(ILI9341_WHITE);

} // END VOID SHOW-FREQUENCY

void setfreq () {
   hilfsf = bands[band].freq*10000000*MASTER_CLK_MULT*SI5351_FREQ_MULT;
//   hilfsf = bands[band].freq*10000000UL*MASTER_CLK_MULT*SI5351_FREQ_MULT;
   hilfsf = hilfsf / calibration_factor;
   si5351.set_freq(hilfsf, SI5351_CLK2);
}   

void buttons() {
      button1.update(); // BAND --
      button2.update(); // BAND ++
      button3.update(); // change DEMOD_MODE
      button4.update(); // change sample rate
      button5.update();
            if ( button1.fallingEdge()) { 
                if(--band < FIRST_BAND) band=LAST_BAND; // cycle thru radio bands 
                set_band();       
            }
            if ( button2.fallingEdge()) { 
                if(++band > LAST_BAND) band=FIRST_BAND; // cycle thru radio bands
                set_band(); 
            }
            if ( button3.fallingEdge()) {  // cycle through DEMOD modes
                if(++demod_mode > DEMOD_MAX) demod_mode = DEMOD_MIN; // cycle thru demod modes
                setup_mode(demod_mode); 
            }
            if ( button4.fallingEdge()) { // cycle thru sample rates
                if(++band > LAST_BAND) band=FIRST_BAND; // cycle thru radio bands
                set_band(); 
            }
            if ( button5.fallingEdge()) { // cycle thru tune steps
                if(++tune_stepper > TUNE_STEP_MAX) tune_stepper = TUNE_STEP_MIN; // cycle thru radio bands
                if(tune_stepper == 0) tunestep = 5000 / 4;
                else if (tune_stepper == 1) tunestep = 1;
                else if (tune_stepper == 2) tunestep = 100 / 4; 
            }
}

void set_band () {
//         show_band(bands[band].name); // show new band
         setup_mode(bands[band].mode);
         sgtl5000_1.lineInLevel((float)bands[band].RFgain/10, (float) bands[band].RFgain/10);
//         setup_RX(bands[band].mode, bands[band].bandwidthU, bands[band].bandwidthL);  // set up the audio chain for new mode
         setfreq();
         show_frequency(bands[band].freq - IF_FREQ);
}

/* #########################################################################
 *  
 *  void setup_mode
 *  
 *  set up radio for RX modes - USB, LSB
 * ######################################################################### 
 */

void setup_mode(int MO) {
  switch (MO)  {

    case DEMOD_LSB:
        if (bands[band].bandwidthU != 0) bands[band].bandwidthL = bands[band].bandwidthU;
        bands[band].bandwidthU = 0;
    break;
    case DEMOD_USB:
        if (bands[band].bandwidthL != 0) bands[band].bandwidthU = bands[band].bandwidthL;
        bands[band].bandwidthL = 0;
    break;
    case DEMOD_AM:
        if (bands[band].bandwidthU >= bands[band].bandwidthL) 
            bands[band].bandwidthL = bands[band].bandwidthU;
            else bands[band].bandwidthU = bands[band].bandwidthL;
    break;
/*    case modeDSB:
        if (bands[band].bandwidthU >= bands[band].bandwidthL) 
            bands[band].bandwidthL = bands[band].bandwidthU;
            else bands[band].bandwidthU = bands[band].bandwidthL;
    break;
    case modeStereoAM:
        if (bands[band].bandwidthU >= bands[band].bandwidthL) 
            bands[band].bandwidthL = bands[band].bandwidthU;
            else bands[band].bandwidthU = bands[band].bandwidthL;
    break;
    */
  }
    show_bandwidth(demod_mode, LP_F_help);
} // end void setup_mode

void encoders () {
  static long encoder_pos=0, last_encoder_pos=0;
  long encoder_change;
  
  // tune radio and adjust settings in menus using encoder switch  
  encoder_pos = tune.read();
  
  if (encoder_pos != last_encoder_pos){
      encoder_change=(encoder_pos-last_encoder_pos);
      last_encoder_pos=encoder_pos;
 
    if (((band == BAND_LW) || (band == BAND_MW)) && (tune_stepper == TUNE_STEP1)) 
        tunestep = 9000 / 4;
    
    if (((band != BAND_LW) && (band != BAND_MW)) && (tunestep == 9000 / 4))
      tunestep = TUNE_STEP1;

    bands[band].freq+=encoder_change*tunestep;  // tune the master vfo 
    if (bands[band].freq > F_MAX) bands[band].freq = F_MAX;
    if (bands[band].freq < F_MIN) bands[band].freq = F_MIN;
    setfreq();
    show_frequency(bands[band].freq - IF_FREQ);
  }
}

