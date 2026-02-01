// CW Macro system
// Storage format: first byte = length (0xff = empty), remaining bytes = characters
// Each character byte: bits 0-6 = character, bit 7 = space before character
// Supported characters: A-Z, 0-9, '/', '?'

#ifndef APP_CWMACRO_H
#define APP_CWMACRO_H

#include <stdint.h>
#include <stdbool.h>

// Keep full 40-character payload and store a checksum byte after the payload
#define CW_MACRO_MAX_LEN 40  // payload bytes
#define CW_MACRO_COUNT 2
// Block layout: [0]=len|SIG, [1..40]=payload bytes (encoded), [41]=checksum
#define CW_MACRO_BLOCK_SIZE 42

// Signature is stored in the length byte (high bit). Checksum is stored in the
// extra byte at the end of the 42-byte block (offset 41).
#define CW_MACRO_SIG 0x80
#define CW_MACRO_CHECKSUM_OFFSET (CW_MACRO_BLOCK_SIZE - 1)

// EEPROM addresses for macros (each uses CW_MACRO_BLOCK_SIZE bytes)
// We reuse the DTMF contacts region (0x1C00..0x1CFF), DTMF calling must be disabled

#define CW_MACRO1_EEPROM_ADDR 0x1C00
#define CW_MACRO2_EEPROM_ADDR (CW_MACRO1_EEPROM_ADDR + CW_MACRO_BLOCK_SIZE)  /* ensures blocks don't overlap */

// Encoding/Decoding helper macros
#define CW_MACRO_ENCODE(ch, hasSpace) ((hasSpace) ? ((ch) | 0x80) : (ch))
#define CW_MACRO_HAS_SPACE(byte) (((byte) & 0x80) != 0)
#define CW_MACRO_GET_CHAR(byte) ((byte) & 0x7F)

// Load a macro from EEPROM into buffer
// Returns actual character count (without considering spaces)
uint8_t CW_LoadMacro(uint8_t macroIndex, char *buffer, uint8_t bufferSize);

// Save a macro to EEPROM from buffer
void CW_SaveMacro(uint8_t macroIndex, const char *buffer, uint8_t length);

// Get the character count for a macro (reads length byte from EEPROM)
uint8_t CW_GetMacroLength(uint8_t macroIndex);

// Validate if a character is allowed in CW macros (A-Z, 0-9, '/', '?')
bool CW_ValidateChar(char ch);

// Format macro for display (first N chars)
// Returns number of characters copied
uint8_t CW_FormatMacroDisplay(uint8_t macroIndex, char *display, uint8_t maxChars);

// Playback helper: given a decoded character, return pattern (LSB-first) and element length
// Playback helper: given a decoded character, return pattern (LSB-first) and element length
bool CW_GetMorseForChar(char ch, uint8_t *pattern, uint8_t *length);

// Morse code encoder - receives element events from keyer FSM
typedef enum {
	CW_ELEMENT_DIT = 0,
	CW_ELEMENT_DAH = 1,
	CW_ELEMENT_INTER_CHAR_SPACE = 2,  // End of character
	CW_ELEMENT_INTER_WORD_SPACE = 3   // End of word (space before next char)
} CW_ElementType_t;

// Process a morse code element from the keyer
void CW_EncoderProcessElement(CW_ElementType_t element);

// Recording state management
extern bool gCW_Recording;
extern uint8_t gCW_RecordMacroIndex;  // Which macro (0 or 1)
extern uint8_t gCW_RecordBuffer[CW_MACRO_MAX_LEN];  // Encoded characters
extern uint8_t gCW_RecordLength;  // Current length
extern bool gCW_RecordNewChar;  // Flag: new character ready for display update

// Start recording a macro
void CW_StartRecording(uint8_t macroIndex);

// Stop recording and save
void CW_StopRecording(void);

// Get the recording display string (last 9 chars + cursor position)
// Returns cursor position (0-9)
uint8_t CW_GetRecordingDisplay(char *display, uint8_t maxChars);

// TX character display buffer (for showing what's being transmitted)
#define CW_TX_DISPLAY_SIZE 16
extern char gCW_TX_Display[CW_TX_DISPLAY_SIZE];
extern uint8_t gCW_TX_DisplayIndex;
extern bool gCW_TX_DisplayUpdated;  // Flag: new data needs to be displayed

// Add a decoded character to the TX display buffer
void CW_AddToTxDisplay(char ch, bool hasSpace);

// Clear the TX display buffer
void CW_ClearTxDisplay(void);

#endif
