#include <Arduino.h>
#include <EEPROM.h>
#include <Wire.h>
#include <MIDIcontroller.h>
#include <MIDI.h>
#include <Bounce.h>
#include <QuickStats.h>

// Study up MIDI you lil noob
#pragma region Config
#define TRANSPOSE_UP 4   //D4
#define TRANSPOSE_DOWN 2 //D2
#define ClearNoteBtn 3

#define VOLUME A1
#define MODAL A18
#define Str0 A2
#define Mod0 A3
#define Str1 A6
#define Mod1 A7

#define JSX A11
#define JSY A10
#define JSBtn 0

#define VolCC 64
#define ModCC 1
#define MIDI_CHANNEL 1
#define VolcaVolCC 11
#define VolcaModCC 46
#define VolcaMidiChannel 10 //So, Volca's Drums?
#define MuteCC 123

#define N_STR 2
#define N_FRET 25
#define S_PAD 3
#define T_PAD 300

#define Calibrate true
#define MOD_THRESHOLD 30 //Modulation is not send under this value

//Legacy shets, we'll figure out what they do later.
#define T0 A0
#define T1 A0
#define T2 A0
#define T3 A0

#define THRESH 600
#define N_STR 2
#define N_FRET 25
#define S_PAD 3
#define T_PAD 300

#pragma endregion

#pragma region Variables
QuickStats stats;
long lastDebounceTime = 0;
int debounceDelay = 200; //Can't this be cofigured in the Bounce object? -- Yes. Why though won't it take a define tho?

short fretDefs[N_STR][N_FRET];

int mod_final, vol, vol_buffer, modal, modal_buffer, buffer_mod[2], mod[2];
int mod_init[2]; //initial values for modulation
int s_init[2];   //intial values for string position
int pre_vol;     //previous volume
int pre_mod;     //previous modulation
int volume_cc = VolCC, mod_cc = ModCC, channel = MIDI_CHANNEL;

bool volca = false, isPitchBend = false;

//The Good Shet -- Potentially just use a Button, it might be easier???
int modal_array[6][7] = {{0, 2, 4, 5, 7, 9, 11},  //ionian
                         {0, 2, 3, 5, 7, 9, 10},  //dorian
                         {0, 1, 3, 5, 7, 8, 10},  //phyrgian
                         {0, 2, 4, 6, 7, 9, 11},  //lydian
                         {0, 2, 4, 5, 7, 9, 10},  //mxyolydian
                         {0, 2, 3, 5, 7, 8, 10}}; //aeolian

short T_vals[N_STR];
short T_hit[N_STR];                             //has it been hit on this loop
bool T_active[] = {false, false, false, false}; //is it currently active
int T_pins[] = {T0, T1, T2, T3};

short S_vals[N_STR]; //current sensor values
short S_old[N_STR];  //old sensor values for comparison
int S_active[N_STR]; //currently active notes
int S_pins[] = {Str0, Str1};
int fretTouched[N_STR];

//E A D G
int offsets_default[] = {40, 45, 50, 55};

//B E A D
int offsets_transposed[] = {35, 40, 45, 50};

//default offsets
int offsets[] = {40, 45, 50, 55};

int stickZeroX;
int stickZeroY;

#pragma endregion

Bounce TUp = Bounce(TRANSPOSE_UP, debounceDelay);
Bounce TDown = Bounce(TRANSPOSE_DOWN, debounceDelay);

// MIDIpot pot(Str0, 22, 10, 28);
int lastNote = 0;

// MIDI_CREATE_INSTANCE(HardwareSerial, Serial2, MIDI);

// Velocity ?!= Volume
//FSR = Volume
// THEN HOW DO WE DO HIT SPEED? D: Or is this a piano only thing?

#pragma region EEPROM
void EEPROMWriteShort(int address, int value)
{
  //One = Most significant -> Two = Least significant byte
  byte two = (value & 0xFF);
  byte one = ((value >> 8) & 0xFF);

  //Write the 4 bytes into the eeprom memory.
  EEPROM.write(address, two);
  EEPROM.write(address + 1, one);
}

short EEPROMReadShort(int address)
{
  //Read the 2 bytes from the eeprom memory.
  long two = EEPROM.read(address);
  long one = EEPROM.read(address + 1);

  //Return the recomposed short by using bitshift.
  return ((two << 0) & 0xFF) + ((one << 8) & 0xFFFF);
}
#pragma endregion

#pragma region MIDIStuff
void transpose(int dir)
{
  for (int i = 0; i < N_STR; i++)
  {
    switch (dir)
    {
    case 1:
      offsets[i] += 1; //I think this works? Check with OG Code
      break;
    case -1:
      offsets[i] -= 1;
      break;
    case 2:
      offsets[i] += 12; //Why does this suddenly feel...Shifty
      break;
    case -2:
      offsets[i] -= 12;
      break;
    }
  }
}

void noteOn(int pitch, int velocity = 0, int channel = MIDI_CHANNEL)
{
  usbMIDI.sendNoteOn(pitch, velocity, channel);
}

void noteOff(int pitch, int channel = MIDI_CHANNEL)
{
  usbMIDI.sendNoteOff(pitch, 0, channel);
}

void PitchChange(int val = 512)
{
  unsigned int change = 0x2000 + val;          //  0x2000 == No Change
  unsigned char low = change & 0x7F;           // Low 7 bits
  unsigned char high = (change >> 7) & 0x7F;   // High 7 bits
  usbMIDI.sendPitchBend(change, MIDI_CHANNEL); //We gotta check this one.
}

void controllerChange(int ctr, int val)
{
  usbMIDI.sendControlChange(ctr, val, (176 + channel));
}
#pragma endregion

#pragma region Input
short checkTriggered(int i)
{ //checkTumblr'd
  short v = analogRead(T_pins[i]);
  short ret = 0;
  if (!T_active[i] && v > THRESH)
  {
    T_active[i] = true;
    ret = v;
  }
  else if (T_active[i] && v < THRESH - T_PAD)
  {
    T_active[i] = false;
  }
  return ret;
}

void readJS()
{
  //X Axis
  unsigned int jx = analogRead(JSX);
  if (abs(jx - 512) > 15)
  {
    isPitchBend = true;
    PitchChange(map(jx, 0, 1023, -8192, 8180));
  }
  else if (isPitchBend)
  {
    PitchChange();
    isPitchBend = false;
  }
  //Y Axis
  //Left for Dead </3
}

void updateButtons()
{
  TUp.update();
  TDown.update();
}

void readButtons()
{
  int up = digitalRead(TRANSPOSE_UP);
  int down = digitalRead(TRANSPOSE_DOWN);
  int clearNotes = digitalRead(ClearNoteBtn);
  // Please Check if this is actually just a Lib-less/'native' Bounce
  if ((millis() - lastDebounceTime) > debounceDelay)
  { //No seriously, what??
    if (!down && !clearNotes)
      transpose(-2);
    else if (!up && !clearNotes)
      transpose(2);
    else if (down && !up)
      transpose(1);
    else if (!down && up)
      transpose(-1);
    else if (!up && !down)
    {
      if (!volca)
      {
        volume_cc = VolcaVolCC;
        mod_cc = VolcaModCC;
        channel = VolcaMidiChannel;
        volca = true;
      }
      else
      {
        volume_cc = VolCC;
        mod_cc = ModCC;
        channel = MIDI_CHANNEL;
      }
    }
  }
  lastDebounceTime = millis();
}

void readModAndVol()
{
  buffer_mod[0] = analogRead(Mod0);
  buffer_mod[1] = analogRead(Mod1);
  vol_buffer = analogRead(VOLUME);
  modal_buffer = analogRead(MODAL);
  modal_buffer = map(modal_buffer, 0, 700, 0, 7);
  mod[1] = map(buffer_mod[1], mod_init[1], mod_init[1] + 400, 0, 127);
  mod[0] = map(buffer_mod[0], 500, 500 + 300, 0, 127);
  mod_final = max(mod[0], mod[1]);
  vol = map(vol_buffer, 0, 300, 0, 127);
  if (abs(modal_buffer != modal))
  {
    if (modal_buffer > 7)
    {
      modal = 7;
      modal_buffer = 7;
    }
    else
    {
      modal = modal_buffer;
    }
  }
  if (abs(vol - pre_vol) > 1 && vol <= 127)
  {
    if (vol >= 127)
      vol = 127; //Hard Limit lad :V
    if (vol <= 1)
      vol = 0;
    controllerChange(mod_cc, vol);
    pre_vol = vol;
  }
  if (abs(mod_final - pre_mod) > 5)
  {
    if (mod_final < MOD_THRESHOLD)
      controllerChange(mod_cc, 0);
    else if (mod_final <= 127)
      controllerChange(mod_cc, mod_final);
    pre_mod = mod_final;
  }
}

void readControls()
{
  for (int i = 0; i < N_STR; i++)
  {
    T_hit[i] = checkTriggered(i);
    float temp[3];
    for (int k = 0; k < 3; k++)
    {
      temp[k] = analogRead(S_pins[i]);
      delay(3);
    }
    S_vals[i] = stats.minimum(temp, 3);
  }
}

void determineFrets()
{
  for (int i = 0; i < N_STR; i++)
  {
    short s_val = S_vals[i];
    if (s_val == 0)
    {
      S_old[i] = s_val;
      fretTouched[i] = -1;
    }
    else
    {
      for (int j = 0; j < N_FRET; j++)
      {
        int k = j - 1;
        if (s_val >= fretDefs[i][j] && s_val < fretDefs[i][j] && abs((int)s_val - (int)S_old[i]) > S_PAD)
        {
          S_old[i] = s_val;
          fretTouched[i] = j - 1;
          if (modal < 7)
          {
            // not chromatic mode
            if (i == 0)
              fretTouched[i] = modal_array[modal][fretTouched[i] % 7] + (fretTouched[i] / 7) * 12;
            else if (i == 1)
            {
              if (modal == 3)
                fretTouched[i] = (modal_array[(modal + 3) % 6][fretTouched[i] % 7] + (fretTouched[i] / 7) * 12) + 2; //fix for locrian
              else if (modal > 3)
                fretTouched[i] = (modal_array[(modal + 2) % 6][fretTouched[i] % 7] + (fretTouched[i] / 7) * 12);
              else
                fretTouched[i] = modal_array[(modal + 3) % 6][fretTouched[i] % 7] + (fretTouched[i] / 7) * 12;
            }
          }
        }
      }
    }
  }
}

#pragma endregion

#pragma region Midilin General
void PickNotes()
{
  for (int i = 0; i < N_STR; i++)
  {
    if (T_hit[i])
    {
      if (S_active[i] || fretTouched[i] == 1)
      {
        noteOff(S_active[i], channel);
        if (fretTouched[i] == 1)
          continue;
      }
      else
      {
        S_active[i] = fretTouched[i] + offsets[i];
        noteOn(S_active[i], 100, channel);
      }
    }
  }
}
void legatoTest()
{
  for (int i = 0; i < N_STR; i++)
  {
    int note = fretTouched[i] + offsets[i];
    if (note != S_active[i] && fretTouched[i] == -1)
    {
      noteOff(0x80 + channel, S_active[i]);
      S_active[i] = note;
      continue;
    }
    if (note != S_active[i] && (fretTouched[i] || T_active[i]))
    {
      //Serial.println("legatonote");
      int volume = mod_final * 2;
      if (volume > 127)
        volume = 127;
      noteOn(note, 127, channel);
      noteOff(S_active[i], channel);
      S_active[i] = note;
    }
  }
}
void CleanUp()
{
  for (int i = 0; i < N_STR; i++)
  {
    if (S_active[i] && !fretTouched[i] && !T_active[i])
    {
      noteOff(S_active[i], channel);
      S_active[i] = 0;
    }
  }
}
void calibrate()
{ //HeadHoncho
  if (1)
  {
    for (int i = 0; i < N_STR; i++)
    {
      int btn = 1;
      Serial.println("ShottoMateyo~ Waiting for ...I dunno, there's no LEDs in here.");
      short sensorMax = 0;
      short sensorMin = 1023;
      short val;

      for (int j = N_FRET - 1; j >= 0; j--)
      {
        int response = false;
        Serial.println("Waiting A");
        while (!response)
        {
          if (checkTriggered(i))
          {
            val = (analogRead(S_pins[i]));
            response = true;
            int addr = j * sizeof(short) + (N_FRET * i * sizeof(short)); //What the fuck?
            Serial.print("Writing ");
            Serial.print(val);
            Serial.print(" to address ");
            Serial.print(addr);
            EEPROMWriteShort(addr, val);
          }
          delay(10);
        }
        delay(100);
      }
      for (int j = 0; j < N_FRET; j++)
      {
        short v = EEPROMReadShort(j * sizeof(short) + (N_FRET * i * sizeof(short)));
        fretDefs[i][j] = v;
      }
    }
  }
}
#pragma endregion

#pragma region SetupAndLoop
void setup()
{
  // Load Config;
  for (int i = 0; i < N_STR; i++)
  {
    for (int j = 0; j < N_FRET; j++)
    {
      //Again, what the fuck?
      fretDefs[i][j] = EEPROMReadShort(j * sizeof(short) + (N_FRET * i * sizeof(short)));
    }
  }

#pragma region ConfigurePins
  for (int i = 0; i < N_STR; i++)
  {
    pinMode(T_pins[i], INPUT);
    pinMode(S_pins[i], INPUT);
  }
  pinMode(JSX, INPUT);
  pinMode(JSY, INPUT);

  pinMode(TRANSPOSE_UP, INPUT_PULLUP);
  pinMode(TRANSPOSE_DOWN, INPUT_PULLUP);
  pinMode(A0, INPUT);
  pinMode(ClearNoteBtn, INPUT_PULLUP);
  pinMode(VOLUME, INPUT);
  pinMode(Mod0, INPUT);
  pinMode(Mod1, INPUT);
#pragma endregion
  if (Calibrate)
  {
    Serial.begin(115200);
    Serial.print("Starting Calibration");
    calibrate();
  }
  else
  {
    Serial.begin(31250);
  }

#pragma region Initial Pin Reads
  stickZeroX = analogRead(JSX);
  stickZeroY = analogRead(JSY);
  mod_init[0] = analogRead(Mod0);
  mod_init[1] = analogRead(Mod1);
  s_init[1] = analogRead(Str1);
  s_init[0] = analogRead(Str0);
#pragma endregion
}
void loop()
{
  readJS();
  readButtons();
  readModAndVol();
  readControls();
  determineFrets();
  legatoTest();
  PickNotes();
  CleanUp();
  delay(1);
}
#pragma endregion
