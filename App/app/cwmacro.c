// CW Macro system implementation

#include "app/cwmacro.h"
#include "driver/eeprom.h"
#include "driver/uart.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "ui/main.h"
#include <string.h>

// Debug logging control - set to 1 to enable encoder debug output
#define CW_ENCODER_DEBUG 0

// Morse code lookup table
// Pattern: LSB first, 0=dit, 1=dah
typedef struct {
	char ch;
	uint8_t length;   // Number of elements (1-6)
	uint8_t pattern;  // Bit pattern: 0=dit, 1=dah, LSB first
} MorseCode_t;

static const MorseCode_t MORSE_TABLE[] = {
	// Letters A-Z (LSB first: bit 0 = first element, 0=dit, 1=dah)
	{'A', 2, 0b10},      // .-
	{'B', 4, 0b0001},    // -...
	{'C', 4, 0b0101},    // -.-.
	{'D', 3, 0b001},     // -..
	{'E', 1, 0b0},       // .
	{'F', 4, 0b0100},    // ..-.
	{'G', 3, 0b011},     // --.
	{'H', 4, 0b0000},       // ....
	{'I', 2, 0b00},       // ..
	{'J', 4, 0b1110},    // .---
	{'K', 3, 0b101},     // -.-
	{'L', 4, 0b0010},    // .-..
	{'M', 2, 0b11},      // --
	{'N', 2, 0b01},      // -.
	{'O', 3, 0b111},     // ---
	{'P', 4, 0b0110},    // .--.
	{'Q', 4, 0b1011},    // --.-
	{'R', 3, 0b010},     // .-.
	{'S', 3, 0b000},       // ...
	{'T', 1, 0b1},       // -
	{'U', 3, 0b100},     // ..-
	{'V', 4, 0b1000},    // ...-
	{'W', 3, 0b110},     // .--
	{'X', 4, 0b1001},    // -..-
	{'Y', 4, 0b1101},    // -.--
	{'Z', 4, 0b0011},    // --..
	// Numbers 0-9
	{'0', 5, 0b11111},   // -----
	{'1', 5, 0b11110},   // .----
	{'2', 5, 0b11100},   // ..---
	{'3', 5, 0b11000},   // ...--
	{'4', 5, 0b10000},   // ....-
	{'5', 5, 0b00000},       // .....
	{'6', 5, 0b00001},       // -....
	{'7', 5, 0b00011},      // --...
	{'8', 5, 0b00111},     // ---..
	{'9', 5, 0b01111},    // ----.
	// Punctuation
	{'/', 5, 0b01001},    // -..-.
	{'?', 6, 0b001100},    // ..--..
};

#define MORSE_TABLE_SIZE (sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]))

// Encoder state machine
static uint8_t s_encoder_pattern = 0;  // Accumulated pattern
static uint8_t s_encoder_length = 0;   // Number of elements in pattern
static bool s_encoder_space_pending = false;  // Word space before next char

// Recording state
bool gCW_Recording = false;
uint8_t gCW_RecordMacroIndex = 0;
uint8_t gCW_RecordBuffer[CW_MACRO_MAX_LEN];
uint8_t gCW_RecordLength = 0;
bool gCW_RecordNewChar = false;
// TX display buffer - shows characters being transmitted
char gCW_TX_Display[CW_TX_DISPLAY_SIZE];
uint8_t gCW_TX_DisplayIndex = 0;
bool gCW_TX_DisplayUpdated = false;
static const uint16_t MACRO_ADDRS[CW_MACRO_COUNT] = {
	CW_MACRO1_EEPROM_ADDR,
	CW_MACRO2_EEPROM_ADDR
};

bool CW_ValidateChar(char ch)
{
	// Valid characters: A-Z, 0-9, '/', '?'
	if (ch >= 'A' && ch <= 'Z')
		return true;
	if (ch >= '0' && ch <= '9')
		return true;
	if (ch == '/' || ch == '?')
		return true;
	return false;
}

uint8_t CW_GetMacroLength(uint8_t macroIndex)
{
	if (macroIndex >= CW_MACRO_COUNT)
		return 0;
	
	uint8_t length;
	EEPROM_ReadBuffer(MACRO_ADDRS[macroIndex], &length, 1);

#if CW_ENCODER_DEBUG
	{
		char buf[64];
		sprintf_(buf, "CW_GetMacroLength: idx=%u raw_len=0x%02x\r\n", macroIndex, length);
		UART_Send(buf, strlen(buf));
	}
#endif
		return 0;
	
	return length;
}

uint8_t CW_LoadMacro(uint8_t macroIndex, char *buffer, uint8_t bufferSize)
{
	if (macroIndex >= CW_MACRO_COUNT || buffer == NULL || bufferSize == 0)
		return 0;
	
	uint8_t length = CW_GetMacroLength(macroIndex);
	if (length == 0) {
		buffer[0] = '\0';
		return 0;
	}
	
	// Read the macro data (skip the length byte)
	uint8_t data[CW_MACRO_MAX_LEN];
	EEPROM_ReadBuffer(MACRO_ADDRS[macroIndex] + 1, data, length < CW_MACRO_MAX_LEN ? length : CW_MACRO_MAX_LEN);
	
	// Decode into buffer with spaces
	uint8_t outPos = 0;
	for (uint8_t i = 0; i < length && outPos < bufferSize - 1; i++) {
		// Check if space precedes this character
		if (CW_MACRO_HAS_SPACE(data[i])) {
			if (outPos < bufferSize - 1) {
				buffer[outPos++] = ' ';
			}
		}
		
		// Get the character
		char ch = CW_MACRO_GET_CHAR(data[i]);
		if (outPos < bufferSize - 1) {
			buffer[outPos++] = ch;
		}
	}
	
	buffer[outPos] = '\0';
	return length;  // Return character count (not including spaces)
}

void CW_SaveMacro(uint8_t macroIndex, const char *buffer, uint8_t length)
{
	if (macroIndex >= CW_MACRO_COUNT || buffer == NULL)
		return;
	
	// Sanity check length
	if (length > CW_MACRO_MAX_LEN)
		length = CW_MACRO_MAX_LEN;

#if CW_ENCODER_DEBUG
	{
		char buf[64];
		sprintf_(buf, "CW_SaveMacro: idx=%u len=%u\r\n", macroIndex, length);
		UART_Send(buf, strlen(buf));
		// Show first few bytes
		for (uint8_t i = 0; i < length && i < 8; i++) {
			sprintf_(buf, "  [%u]=0x%02x (%c%s)\r\n", i, (uint8_t)buffer[i], 
				CW_MACRO_GET_CHAR(buffer[i]), CW_MACRO_HAS_SPACE(buffer[i]) ? " +SPC" : "");
			UART_Send(buf, strlen(buf));
		}
	}
#endif

	// Prepare data to write (40 bytes total)
	uint8_t data[40];
	memset(data, 0xFF, sizeof(data));
	
	// First byte is the character count (or 0xFF for empty)
	data[0] = (length == 0) ? 0xFF : length;
	
	// Copy encoded characters from buffer
	// Buffer contains already-encoded bytes (char with bit 7 set for space)
	for (uint8_t i = 0; i < length; i++) {
		data[i + 1] = buffer[i];
	}
	
	// Write entire 40-byte block to EEPROM
	EEPROM_WriteBuffer(MACRO_ADDRS[macroIndex], data);
}

uint8_t CW_FormatMacroDisplay(uint8_t macroIndex, char *display, uint8_t maxChars)
{
	if (display == NULL || maxChars == 0)
		return 0;
	
	uint8_t length = CW_GetMacroLength(macroIndex);
	if (length == 0) {
	    strcpy(display, "empty");
		return 0;
	}
	
	// Read the macro data (skip the length byte)
	uint8_t data[CW_MACRO_MAX_LEN];
	EEPROM_ReadBuffer(MACRO_ADDRS[macroIndex] + 1, data, length < CW_MACRO_MAX_LEN ? length : CW_MACRO_MAX_LEN);
	
	// Decode into display buffer with spaces, up to 3 lines
	uint8_t outPos = 0;
	uint8_t linePos = 0;
	uint8_t lineCount = 0;
	
	for (uint8_t i = 0; i < length; i++) {
		// Stop if we've filled 3 lines
		if (lineCount >= 3)
			break;
		
		// Check if space precedes this character
		if (CW_MACRO_HAS_SPACE(data[i])) {
			// Check if adding space would exceed line width
			if (linePos >= maxChars) {
				// Start new line
				display[outPos++] = '\n';
				linePos = 0;
				lineCount++;
				if (lineCount >= 3) break;
			}
			display[outPos++] = ' ';
			linePos++;
		}
		
		// Get the character
		char ch = CW_MACRO_GET_CHAR(data[i]);
		
		// Check if we need a new line before this character
		if (linePos >= maxChars) {
			display[outPos++] = '\n';
			linePos = 0;
			lineCount++;
			if (lineCount >= 3) break;
		}
		
		display[outPos++] = ch;
		linePos++;
	}
	
	// Add final newline before char count
	display[outPos++] = '\n';
	
	// Add char count on the 4th line
	int n = sprintf_(display + outPos, "%u chars", length);
	outPos += n;

	display[outPos] = '\0';

#if CW_ENCODER_DEBUG
	{
		char buf[64];
		sprintf_(buf, "CW_FormatMacro result: outPos=%u lineCount=%u\r\n", outPos, lineCount);
		UART_Send(buf, strlen(buf));
		sprintf_(buf, "Display string: [%s]\r\n", display);
		UART_Send(buf, strlen(buf));
	}
#endif

	return outPos;
}

// Decode accumulated pattern into a character
static char CW_DecodePattern(uint8_t pattern, uint8_t length)
{
	if (length == 0 || length > 6)
		return 0;
	
	for (unsigned int i = 0; i < MORSE_TABLE_SIZE; i++) {
		if (MORSE_TABLE[i].length == length && MORSE_TABLE[i].pattern == pattern) {
			return MORSE_TABLE[i].ch;
		}
	}
	
	return 0;  // Unknown pattern
}

void CW_EncoderProcessElement(CW_ElementType_t element)
{
#if CW_ENCODER_DEBUG
	const char* elem_names[] = {"DIT", "DAH", "INTER_CHAR", "INTER_WORD"};
	if (element <= CW_ELEMENT_INTER_WORD_SPACE) {
		char buf[32];
		sprintf_(buf, "ENC: %s\r\n", elem_names[element]);
		UART_Send(buf, strlen(buf));
	}
#endif

	switch (element) {
	case CW_ELEMENT_DIT:
		// Add a dit (0) to the pattern
		if (s_encoder_length < 6) {
			// Pattern LSB first, so no shift needed for new bit
			s_encoder_length++;
		}
		break;
		
	case CW_ELEMENT_DAH:
		// Add a dah (1) to the pattern
		if (s_encoder_length < 6) {
			s_encoder_pattern |= (1 << s_encoder_length);
			s_encoder_length++;
		}
		break;
		
	case CW_ELEMENT_INTER_CHAR_SPACE:
		// End of character - decode and emit
#if CW_ENCODER_DEBUG
		{
			char buf[64];
			sprintf_(buf, "DECODE: pattern=0x%02x len=%d\r\n", s_encoder_pattern, s_encoder_length);
			UART_Send(buf, strlen(buf));
		}
#endif
		if (s_encoder_length > 0) {
			char ch = CW_DecodePattern(s_encoder_pattern, s_encoder_length);
#if CW_ENCODER_DEBUG
			{
				char buf[64];
				sprintf_(buf, "  Result: ch='%c' (%d) valid=%d\r\n", 
					ch ? ch : '?', ch, ch != 0 && CW_ValidateChar(ch));
				UART_Send(buf, strlen(buf));
			}
#endif
			if (ch != 0 && CW_ValidateChar(ch)) {
#if CW_ENCODER_DEBUG
				// Print decoded character for debug
				char buf[32];
				if (s_encoder_space_pending) {
					sprintf_(buf, "CW: (space) [%c]\r\n", ch);
				} else {
					sprintf_(buf, "CW: [%c]\r\n", ch);
				}
				UART_Send(buf, strlen(buf));
#endif
				
				// Emit to recorder if recording
				if (gCW_Recording && gCW_RecordLength < CW_MACRO_MAX_LEN) {
					gCW_RecordBuffer[gCW_RecordLength++] = CW_MACRO_ENCODE(ch, s_encoder_space_pending);
					gCW_RecordNewChar = true;
					
					// Auto-complete if we hit the limit
					if (gCW_RecordLength >= CW_MACRO_MAX_LEN) {
						CW_StopRecording();
					}
				}
				// Add to TX display buffer when transmitting (not recording)
				else if (!gCW_Recording) {
					CW_AddToTxDisplay(ch, s_encoder_space_pending);
				}
			}
			
			// Reset for next character
			s_encoder_pattern = 0;
			s_encoder_length = 0;
			s_encoder_space_pending = false;
		}
		break;
		
	case CW_ELEMENT_INTER_WORD_SPACE:
		// Mark that next character should have space before it
		// Also reset encoder state for next character
		s_encoder_pattern = 0;
		s_encoder_length = 0;
		s_encoder_space_pending = true;
		break;
	}
}

// Recording functions
void CW_StartRecording(uint8_t macroIndex)
{
	if (macroIndex >= CW_MACRO_COUNT)
		return;
	
	gCW_RecordMacroIndex = macroIndex;
	gCW_RecordLength = 0;
	gCW_RecordNewChar = false;
	gCW_Recording = true;
	
	// Reset encoder state
	s_encoder_pattern = 0;
	s_encoder_length = 0;
	s_encoder_space_pending = false;
}

void CW_StopRecording(void)
{
	if (!gCW_Recording)
		return;
	
	// Save the recorded macro
	CW_SaveMacro(gCW_RecordMacroIndex, (const char *)gCW_RecordBuffer, gCW_RecordLength);
	
	gCW_Recording = false;
	gCW_RecordNewChar = false;
}

bool CW_IsRecording(void)
{
	return gCW_Recording;
}

void CW_AddToTxDisplay(char ch, bool hasSpace)
{
#if CW_ENCODER_DEBUG
	char buf[64];
	sprintf_(buf, "AddToTx: ch='%c' space=%d idx=%d\r\n", ch, hasSpace, gCW_TX_DisplayIndex);
	UART_Send(buf, strlen(buf));
#endif
	
	// Add space first if needed
	if (hasSpace) {
		if (gCW_TX_DisplayIndex >= CW_TX_DISPLAY_SIZE - 1) {
			// Buffer full, shift left by 1 to make room
			memmove(gCW_TX_Display, gCW_TX_Display + 1, CW_TX_DISPLAY_SIZE - 2);
			gCW_TX_DisplayIndex = CW_TX_DISPLAY_SIZE - 2;
		}
		gCW_TX_Display[gCW_TX_DisplayIndex++] = ' ';
		gCW_TX_Display[gCW_TX_DisplayIndex] = '\0';
	}
	
	// Add character
	if (gCW_TX_DisplayIndex >= CW_TX_DISPLAY_SIZE - 1) {
		// Buffer full, shift left by 1 to make room
		memmove(gCW_TX_Display, gCW_TX_Display + 1, CW_TX_DISPLAY_SIZE - 2);
		gCW_TX_DisplayIndex = CW_TX_DISPLAY_SIZE - 2;
	}
	gCW_TX_Display[gCW_TX_DisplayIndex++] = ch;
	gCW_TX_Display[gCW_TX_DisplayIndex] = '\0';
	
	// Trigger main display update
	gUpdateDisplay = true;
	
#if CW_ENCODER_DEBUG
	sprintf_(buf, "  Buffer now: \"%s\" (len=%d)\r\n", gCW_TX_Display, strlen(gCW_TX_Display));
	UART_Send(buf, strlen(buf));
#endif
}

void CW_ClearTxDisplay(void)
{
#if CW_ENCODER_DEBUG
	char buf[64];
	sprintf_(buf, "ClearTxDisplay called\r\n");
	UART_Send(buf, strlen(buf));
#endif
	memset(gCW_TX_Display, 0, CW_TX_DISPLAY_SIZE);
	gCW_TX_DisplayIndex = 0;
}

uint8_t CW_GetRecordingDisplay(char *display, uint8_t maxChars)
{
	if (display == NULL || maxChars == 0 || !gCW_Recording)
		return 0;
	
	uint8_t cursor_pos = 0;
	uint8_t start_idx = 0;
	
	// Build display string from the end, working backwards to fit in maxChars
	// Display width is limited (typically 16 chars for full screen width)
	// We need to scroll when display would exceed ~9 chars to prevent wrapping
	
	uint8_t outPos = 0;
	
	// First pass: calculate how much will fit in maxChars
	// Start from the end and work backwards
	if (gCW_RecordLength > 0) {
		// Calculate display length from the end
		uint8_t test_pos = 0;
		for (int i = gCW_RecordLength - 1; i >= 0 && test_pos < (maxChars - 2); i--) {
			// Count space if needed
			if (CW_MACRO_HAS_SPACE(gCW_RecordBuffer[i])) {
				test_pos++;
			}
			test_pos++;  // The character itself
			
			// Stop scrolling if we fit in ~9 display positions
			if (test_pos <= 9) {
				start_idx = i;
			} else {
				break;
			}
		}
	}
	
	// Decode characters for display starting from start_idx
	for (uint8_t i = start_idx; i < gCW_RecordLength && outPos < maxChars - 2; i++) {
		// Add space if needed
		if (CW_MACRO_HAS_SPACE(gCW_RecordBuffer[i])) {
			if (outPos < maxChars - 2) {
				display[outPos++] = ' ';
			}
		}
		
		// Add character
		char ch = CW_MACRO_GET_CHAR(gCW_RecordBuffer[i]);
		if (outPos < maxChars - 2) {
			display[outPos++] = ch;
		}
	}
	
	// Add cursor placeholder if room
	if (outPos < maxChars - 1 && gCW_RecordLength < CW_MACRO_MAX_LEN) {
		display[outPos++] = '_';
	}
	
	display[outPos] = '\0';
	
	// Cursor position is at the underscore (end of string - 1)
	if (outPos > 0 && display[outPos - 1] == '_') {
		cursor_pos = outPos - 1;
	} else {
		cursor_pos = outPos;
	}
	
	return cursor_pos;
}
