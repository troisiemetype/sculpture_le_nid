#include <tinyNeoPixel_Static.h>

const uint8_t PIN_NEO = 4;

const uint8_t NUM_NEO = 41;

uint8_t pixels[NUM_NEO * 3];

tinyNeoPixel leds = tinyNeoPixel(NUM_NEO, PIN_NEO, NEO_RGB, pixels);

const uint16_t SRAM_START = 0x0000;		// SRAM start is 0x3800, but registers can be used as well, some have undecided reset value.
const uint16_t SRAM_END = 0x3F00;		// 

// Fades are in ms. Converted to PWM cycles in code, and rounded to suit timer overflow frequency
const uint16_t FADE_XFAST = 50;
const uint16_t FADE_FAST = 150;
const uint16_t FADE_MID = 500;
const uint16_t FADE_SLOW = 1500;

const uint8_t STATE_QUEUE_SIZE = 32;
const uint8_t PATTERN_QUEUE_SIZE = 32;

uint32_t seed;

struct color_t{
	uint16_t color;
	int8_t change;
} R, G, B;

// Those are the states to be queued.
enum{
	IDLE = 0,
	FADE,
	WAIT,
};

// Structures for different states, to be inserted into the global state structure.
struct wait_t{
	uint16_t delay;
	uint32_t start;
};

struct fade_t{
	int8_t direction;
	uint8_t intPart;
	uint8_t modPart;
	uint8_t current;
	uint8_t end;
	uint8_t acc;
};

struct state_t{
	state_t *next;
	uint8_t state;
	uint8_t size;
	uint8_t color[3];
	union{
		fade_t fade;
		wait_t wait;
	};
} states[STATE_QUEUE_SIZE];

state_t *stateIn;
state_t *stateOut;

struct pattern_t{
	pattern_t *next;
	state_t *entry;
	uint32_t start;
	uint8_t id;
	uint8_t length;
	bool active;
} patterns[PATTERN_QUEUE_SIZE];

pattern_t *patternIn;
pattern_t *patternOut;

bool hasPatternProgrammed = false;

uint32_t last;

void makeSeed(){
	uint8_t *p;

	for(uint16_t i = SRAM_START ; i < SRAM_END; ++i){
		p = (uint8_t*)i;
		seed ^= (uint32_t)(*p) << ((i % 4) * 8);
	}
}

// Alternative to the Arduino random() function.
uint32_t xorshift(uint32_t max = 0){
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;

	if(max != 0) return (uint64_t)seed * max / ((uint32_t)(-1));
	else return seed;
}

void changeValue(int8_t *value, int8_t direction = 1){
	*value = xorshift(128);
	*value *= direction;
}

void testLimit(color_t *color ){
	if(color->color > 65408){
		changeValue(&(color->change), -1);
	} else if(color->color < 127){
		changeValue(&(color->change), 1);
	}
}

void updateLeds(){
	R.color += R.change;
	G.color += G.change;
	B.color += B.change;

	testLimit(&R);
	testLimit(&G);
	testLimit(&B);

	const uint16_t limit = NUM_NEO * 3;

	for(uint16_t i = 0; i < limit; i += 3){
		pixels[i + 0] = R.color >> 10;
		pixels[i + 1] = G.color >> 10;
		pixels[i + 2] = B.color >> 10;
	}
}

void computeLedValue(state_t *state){
	fade_t *fade = &state->fade;

	fade->current += fade->intPart * fade->direction;
	uint8_t accumulator = fade->acc;
	accumulator += fade->modPart;
	if(accumulator < fade->acc) fade->current += fade->direction;
	fade->acc = accumulator;
}

void addPatterns(){
	// Choose a color for this pattern
	uint8_t color[3];

	color[0] = xorshift(256);
	color[1] = xorshift(256);
	color[2] = xorshift(256);

	// Define a random pattern
	uint8_t l = xorshift(12);
	uint8_t m = xorshift(12);

	pattern_t *patternQueue = patternIn;

	uint32_t now = millis();

	for(uint8_t i = 0; i < l; ++i){

		patternQueue->entry = stateIn;

		if(i == 0){
			patternQueue->id = 0;
			patternQueue->start = now += xorshift(9000) + 1000;
		} else {
			patternQueue->id = xorshift(NUM_NEO);
			patternQueue->start = now += xorshift(500);
		}

		patternQueue->length = m;
		patternQueue->active = true;
		patternQueue = patternQueue->next;
	}

	state_t *queue = stateIn;

	for(uint8_t i = 0; i < m; ++i){
		queue->color[0] = color[0];
		queue->color[1] = color[1];
		queue->color[2] = color[2];

		queue->state = FADE;
		queue->fade.current = xorshift(255);
		queue->fade.end = xorshift(255);
		uint16_t duration = xorshift(400) + 100;

		int16_t delta = queue->fade.end - queue->fade.current;
		queue->fade.direction = (delta > 0)?1:-1;

		queue->fade.intPart = delta / duration;
		queue->fade.modPart = delta % duration;
		queue->fade.acc = 0;

		queue->size = l;
		queue = queue->next;
	}
	stateIn = queue;

	// TODO : change for a comparison between queueIn and queueOut.
	hasPatternProgrammed = true;
}

void checkPatterns(){

	uint32_t now = millis();

	pattern_t* queue = patternOut;

	for(uint8_t i = 0; i < PATTERN_QUEUE_SIZE; ++i){
		if(!queue->active){
			if(i == 0) hasPatternProgrammed = false;
//			memset(pixels, 0, sizeof(uint8_t) * NUM_NEO * 3);
			return;
		}

		if(queue->start > now) return;

		state_t *entry = queue->entry;

		if(entry->state == FADE){
			computeLedValue(entry);

			pixels[queue->id * 3 + 0] = (int16_t)entry->color[0] * entry->fade.current / 256;
			pixels[queue->id * 3 + 1] = (int16_t)entry->color[1] * entry->fade.current / 256;
			pixels[queue->id * 3 + 2] = (int16_t)entry->color[2] * entry->fade.current / 256;

			if(entry->fade.current == entry->fade.end){
				if(--queue->length == 0){
					queue->active = false;
					patternOut++;
				}

				if(--entry->size == 0){
					stateOut++;
				}
				queue->entry = entry->next;
			}
		}

		queue = queue->next;
	}
}

void setup(){

	makeSeed();

	pinMode(PIN_NEO, OUTPUT);

	memset(states, 0, sizeof(state_t) * STATE_QUEUE_SIZE);
	memset(patterns, 0, sizeof(pattern_t) * PATTERN_QUEUE_SIZE);

	uint8_t limit = STATE_QUEUE_SIZE - 1;

	for(uint8_t i = 0; i < limit; ++i){
		states[i].next = &states[i + 1];
	}
	states[limit].next = &states[0];
	stateIn = stateOut = &states[0];

	limit = PATTERN_QUEUE_SIZE - 1;

	for(uint8_t i = 0; i < limit; ++i){
		patterns[i].next = &patterns[i + 1];
	}
	patterns[limit].next = &patterns[0];
	patternIn = patternOut = &patterns[0];

	changeValue(&(R.change));
	changeValue(&(G.change));
	changeValue(&(B.change));

	last = millis();

}

void loop(){
	if(!hasPatternProgrammed) addPatterns();

	uint32_t now = millis();

	if(now == last) return;
	last = now;

	updateLeds();

	checkPatterns();

	leds.show();
}
