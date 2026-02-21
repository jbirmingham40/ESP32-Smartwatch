#include "src/eez-flow.h"
#include "Arduino.h"
#include "src/ui.h"
#include "src/actions.h"
#include "src/vars.h"
#include <ctype.h>

/*
  0: "AC"   9: "\n"   18: "+"
  1: "+-"  10: "4"    19: "\n"
  2: "<-"  11: "5"    20: "0"
  3: "/"   12: "6"    21: "."
  4: "\n"  13: "-"    22: "%"
  5: "7"   14: "\n".  23: "="
  6: "8"   15: "1".  
  7: "9"   16: "2"
  8: "X"   17: "3"
*/

typedef enum {
  NONE,
  AC,
  PLUSMINUS,
  BACK,
  DIV,
  MUL,
  MINUS,
  PLUS,
  PERIOD,
  PERCENT,
  EQUALS
} operators_t;

struct button_t {
  char digit;
  operators_t op;
} button;

typedef struct {
    const char *input;
    int pos;
} Parser;

static float parse_addsub(Parser *p);
static float parse_muldiv(Parser *p);
static float parse_term(Parser *p);

static char peek(Parser *p) {
    return p->input[p->pos];
}

static char get(Parser *p) {
    return p->input[p->pos++];
}

static void skip_spaces(Parser *p) {
    while (isspace(peek(p))) get(p);
}

static float parse_number(Parser *p) {
    float n = 0.0f;
    while (isdigit(peek(p))) {
        n = n * 10 + (get(p) - '0');
    }
    if (peek(p) == '.') {
        get(p);
        float frac = 0.1f;
        while (isdigit(peek(p))) {
            n += (get(p) - '0') * frac;
            frac *= 0.1f;
        }
    }
    return n;
}

static float parse_term(Parser *p) {
    skip_spaces(p);
    if (peek(p) == '(') {
        get(p); // consume '('
        float val = parse_addsub(p);
        skip_spaces(p);
        if (peek(p) == ')') get(p); // consume ')'
        return val;
    }
    return parse_number(p);
}

static float parse_muldiv(Parser *p) {
    float value = parse_term(p);
    for (;;) {
        skip_spaces(p);
        char op = peek(p);
        if (op == '%') {
          get(p);
          value /= 100;
          return value;
        } else if (op == 'x' || op == '/') {
            get(p);
            float rhs = parse_term(p);
            if (op == 'x')
                value *= rhs;
            else
                value /= rhs;
        } else {
            return value;
        }
    }
}

static float parse_addsub(Parser *p) {
    float value = parse_muldiv(p);
    for (;;) {
        skip_spaces(p);
        char op = peek(p);
        if (op == '+' || op == '-') {
            get(p);
            float rhs = parse_muldiv(p);
            if (op == '+')
                value += rhs;
            else
                value -= rhs;
        } else {
            return value;
        }
    }
}

float eval(const char *input) {
    Parser p = { input, 0 };
    return parse_addsub(&p);
}

button_t getButtonFromEvent(int buttonId) {
  switch (buttonId) {
    case 0:
      button.op = AC;
      break;
    case 1:
      button.op = PLUSMINUS;
      break;
    case 2:
      button.op = BACK;
      break;
    case 3:
      button.op = DIV;
      break;
    case 4:
      button.digit = '7';
      button.op = NONE;
      break;
    case 5:
      button.digit = '8';
      button.op = NONE;
      break;
    case 6:
      button.digit = '9';
      button.op = NONE;
      break;
    case 7:
      button.digit = 0;
      button.op = MUL;
      break;
    case 8:
      button.digit = '4';
      button.op = NONE;
      break;
    case 9:
      button.digit = '5';
      button.op = NONE;
      break;
    case 10:
      button.digit = '6';
      button.op = NONE;
      break;
    case 11:
      button.op = MINUS;
      break;
    case 12:
      button.digit = '1';
      button.op = NONE;
      break;
    case 13:
      button.digit = '2';
      button.op = NONE;
      break;
    case 14:
      button.digit = '3';
      button.op = NONE;
      break;
    case 15:
      button.op = PLUS;
      break;
    case 16:
      button.digit = '0';
      button.op = NONE;
      break;
    case 17:
      button.op = PERIOD;
      break;
    case 18:
      button.op = PERCENT;
      break;
    case 19: 
      button.op = EQUALS;
      break;
  }

  return button;
}

void strip_trailing_zeros(char *input) {
  // only needed if there is a period
  if(strstr(input, ".") != NULL) {
    int pos;
    for(pos=strlen(input)-1; pos>0 && (input[pos] == '0' /*|| input[pos] == '.'*/); pos--) {
    }
    input[pos+1] = 0;
  }
}

void strip_trailing_dot(char *input) {
  // see if the last char is a period
  if(input[strlen(input)-1] == '.') {
    input[strlen(input)-1] = 0;
  }
}

void action_calculator_button_click(lv_event_t *e) {

  const int MAX_CHARS = 24;
  static char input[MAX_CHARS] = { '0', 0 };
  static char history[MAX_CHARS] = { 0 };
  static bool showingTotal = true;
  char temp[MAX_CHARS];
  bool isPositive = true;

  if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
    uint32_t id = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    button_t button = getButtonFromEvent(id);

    if (strlen(input) + 1 < MAX_CHARS) {  // +1 for the null terminator
      switch (button.op) {
        case NONE:
          if(showingTotal) {
            showingTotal = false;
            strncpy(input, "0", strlen(input));
            input[0] = button.digit;            
          } else {
            input[strlen(input)+1] = 0;
            input[strlen(input)] = button.digit;
          }
          break;
        case AC:
          strncpy(input, "0", strlen(input));
          strncpy(history, "", strlen(history));
          showingTotal = true;
          break;
        case PLUSMINUS:
          isPositive = !isPositive;
          if (isPositive) {
            // shift string left one char
            strncpy(input, input+1, strlen(input+1));
          } else {
            // shift right one char and add -
            strncpy(temp, input, strlen(input)+1);
            input[0] = '-';
            strncpy(input+1, temp, strlen(input));
          }
          break;
        case BACK:
          showingTotal = false;
          if (strlen(input) == 1) {
            strncpy(input, "0", strlen(input));
          } else {
            input[strlen(input) - 1] = '\0';
          }
          break;
        case DIV:
          showingTotal = false;
          strncat(input, "/", strlen(input));
          break;
        case MUL:
          showingTotal = false;
          strncat(input, "x", strlen(input));
          break;
        case MINUS:
          showingTotal = false;
          strncat(input, "-", strlen(input));
          break;
        case PLUS:
          showingTotal = false;
          strncat(input, "+", strlen(input));
          break;
        case PERIOD:
          strncat(input, ".", strlen(input));
          break;
        case PERCENT:
          strncat(input, "%", strlen(input));
          break;
        case EQUALS:
          showingTotal = true;
          strncpy(history, input, strlen(input));
          snprintf(input, sizeof(input), "%.5f", eval(input));
          strip_trailing_zeros(input);
          strip_trailing_dot(input);
          break;
      }
    }
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_CALCULATOR_HISTORY, eez::StringValue(history));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_CALCULATOR_INPUT, eez::StringValue(input));
  } else {
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_CALCULATOR_HISTORY, eez::StringValue(""));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_CALCULATOR_INPUT, eez::StringValue("Error"));
  }
}
