#include <Adafruit_GFX.h>
#include <Adafruit_ST7796S.h>
#include <Fonts/FreeSansBold18pt7b.h>

//Touchscreen control library
#include "FT6336U.h"

//Temporarily acting like an HID-compliant mouse instead of an HID-compliant touchpad
#include "Mouse.h"

//Temporarily using keyboard emulation to hold modifier keys when zooming or sidescrolling
#include "Keyboard.h"

//Define display pin connections
#define TFT_CS        10
#define TFT_RST        9 // Or set to -1 and connect to Arduino RESET pin
#define TFT_DC         8

//Define display PWM brightness connection
#define TFT_PWM 5

//Define screen timeout time and brightness levels (temporary; will put in EEPROM eventually)
#define BR_IDLE 40
#define BR_TOUCH 54
#define BR_GUI 128
#define TIMEOUT_TOUCH 2500

//Define touch pin connections
#define RST_N_PIN 7
#define INT_N_PIN 6

//Define hardware click connections
#define LEFT_BTN 11
#define RIGHT_BTN 12

//Define max tap-to-click time and small allowed move distance
#define MAX_TAPTOCLICK_TIME 300
#define MAX_TAPTOCLICK_DISPLACEMENT_SQUARE 144

//Define max allowed distance to move mouse in a single loop iteration (sets a "speed limit" on the mouse to prevent certain unrealistic jumps)
#define MAX_MOUSE_DELTA_R_SQUARE 40000

//Define click eligibility here
#define CLICK_IDLE 0
#define CLICK_LEFT_PENDING 1
#define CLICK_LEFT_READY 2
#define CLICK_RIGHT_PENDING 3
#define CLICK_RIGHT_READY 4
//Click eligible values mean:
//0: Click ineligible for current touch until all input leaves the screen
//1: While touched: Current touch is eligible to be a left click
//2: Touch just ended; flag state must be reset to 1 upon handling of LEFT click
//3: Click eligible as a right click as both fingers started touching at the same time
//4: Touch just ended; flag state must be reset to 1 upon handling of RIGHT click

//Define multi touch (2 finger gesture) action here
#define MULTI_UNDECIDED 0
#define MULTI_SCROLL 1
#define MULTI_SIDESCROLL 2
#define MULTI_ZOOM 3
//Multi touch actions
//0: Undecided (inactive or newly became 2 finger gesture and not yet decided)
//1: Standard vertical scroll: Use the average (midpoint) of the 2 touch points
//and how far vertically it has moved to determine the amount to scroll
//2: Horizontal scroll: Use the average (midpoint) of the 2 touch points
//and how far horizontally it has moved to determine the amount to scroll
//3: Zoom: Use the change in distance between the 2 touch points to determine
//how much to zoom
#define MULTITOUCH_FORCE_NORMAL_MOUSE_SCROLL //This define is vestigial for now unless the zoom gesture proves itself to be sufficiently glitchy and unfixable

//Define mouse move scaling here
#define MOVE_SCALE 2.5

//Weirdness occurs when touch starts or ends; wait this delay (in ms) before accepting input in such cases
#define TOUCH_DELAY 100

//Define debug(s) here to preprocess in the debug statements (if needed)


//Display controller
Adafruit_ST7796S display(TFT_CS, TFT_DC, TFT_RST);

//Touch controller
FT6336U digitizer(RST_N_PIN, INT_N_PIN);

//Last and current cycle's touch data; none is scaled or rotated
//but all is converted so that fingers are kept identified
//(last1x/y are sometimes unused when last2x/y are used when
//a second (or otherwise even numbered) finger hasn't stopped
//touching the screen when the first one has
uint8_t lastNtouches, lastEvenFingerOnly;
int16_t last1x=0, last1y=0, last2x=0, last2y=0;
uint8_t nTouches, evenFingerOnly;
int16_t x1, y1, x2, y2;
uint32_t startTime1, startTime2;
int16_t startX1, startY1, startX2, startY2;
uint32_t duration1, duration2;

//Extra little bit of mouse moves so no scale gets "lost" to truncated integer division
float partialXMove=0, partialYMove=0;

//Brightness-related variables for screen
uint8_t currentBrightness=0, delta=0, state=0;
uint32_t offTimer=0;

//Anti-touch weirdness timer
uint32_t lastNtouchChange=0;

//Click and scroll related variables
uint8_t clickEligible=0, multiTouchAction=MULTI_UNDECIDED, lastLeftBtn=0, lastRightBtn=0;
float partialScroll=0;

//Later we will calibrate the touch digitizer using these, and even later,
//they will be written to EEPROM to prevent having to recalibrate on every power cycle
//uint8_t calib_x1, calib_y1, calib_x2, calib_y2;

//Later if we want to rotate the screen (probably will also eventually be stored in EEPROM)
//uint8_t rotate = 0; // Current screen orientation (0-3)

void setup()
{
  Serial.begin(115200);

  //Display controller initialize
  display.init(320, 480, 0, 0, ST7796S_RGB);
  display.fillScreen(0); // Clear screen

  //Set screen rotation appropriately
  display.setRotation(3);
  
  //New testing pattern
  display.drawRect(0, 0, display.width()*2/5, display.height()/8, 0xFFE0);
  display.drawRect(display.width()*2/5, 0, display.width()/5, display.height()/8, 0xF81F);
  display.drawRect(display.width()*3/5, 0, display.width()*2/5, display.height()/8, 0x07FF);
  display.fillTriangle(display.width(), 0, display.width()-display.height()/8, 0, display.width(), display.height()/8, 0xF01E);
  display.drawCircle(display.width()-(display.height()/32+2), display.height()/32+2, display.height()/32, 0x0000);

  //Text
  display.setTextSize(2);
  display.setTextColor(0x0000);
  display.setCursor(display.width()-17, 5);
  display.print("G");

  //Touch digitizer initialize
  digitizer.begin();

  //PWM brightness initialize to backlight idle brightness
  pinMode(TFT_PWM, OUTPUT);
  analogWrite(TFT_PWM, BR_IDLE);

  //Setup the hardware click pins
  pinMode(LEFT_BTN, INPUT_PULLUP);
  pinMode(RIGHT_BTN, INPUT_PULLUP);

  //Initialize USB HID mouse and keyboard
  Mouse.begin();
  Keyboard.begin();
}

void loop()
{
  //First we track touch data

  //Take the the ID of touch 1 and positions (These may need reordering later to track particular fingers)
  uint8_t digi_1id=digitizer.read_touch1_id();  
  int16_t digi_1x=digitizer.read_touch1_x(), digi_1y=digitizer.read_touch1_y(),
    digi_2x=digitizer.read_touch2_x(), digi_2y=digitizer.read_touch2_y();

  //Read number of touches (Would be REAL NICE if the FocalTech datasheet for the FT6336U ACTUALLY TOLD YOU that bits [7:4]
  //were reserved instead of having to hunt that down from some other FocalTech digitizer MCU datasheet)
  nTouches = (digitizer.read_td_status() & 0xF);

  //If the digitizer's provided ID for the first touch it reports is 1
  //we need to reverse which variables store each touch
  if (digi_1id)
  {
    x1=digi_2x;
    y1=digi_2y;
    x2=digi_1x;
    y2=digi_1y;
    evenFingerOnly=nTouches;
  }
  else
  {
    x1=digi_1x;
    y1=digi_1y;
    x2=digi_2x;
    y2=digi_2y;
    evenFingerOnly=0;
  }
  
  //Touch(es) ended
  if (nTouches < lastNtouches)
  {
    //A touch ended; ignore input for a bit
    lastNtouchChange=millis();

    //Reset the "not quite full" scroll counting variable
    partialScroll=0;

    //If no touches are left, reset the partial storage x and y variables too
    if (!nTouches)
    {
      partialXMove=0;
      partialYMove=0;
    }

    //Debug mouse prints have this surrounding it
    #ifdef __MOUSE_DEBUG
    
    //Nice few newlines between separate touch movements
    Serial.println();
    Serial.println();
    
    #endif

    //Reset the 2 finger scroll variable and any and all modifier keys
    Keyboard.releaseAll();
    multiTouchAction=MULTI_UNDECIDED;

    //Debug mouse prints have this surrounding it
    #ifdef __GESTURE_DEBUG

    Serial.println("Gesture action reset upon multitouch end");
    
    #endif
    
    if (lastNtouches == 2 && nTouches == 0)
    {
      //Both touches ended; track how long they were and reset their variables
      duration1=millis()-startTime1;
      startX1=-1;
      startY1=-1;
      x1=-1;
      y1=-1;
      duration2=millis()-startTime2;
      startX2=-1;
      startY2=-1;
      x2=-1;
      y2=-1;

      //Multitouch ended; print a delimiter for debug reasons
      #ifdef __MULTITOUCH_DEBUG
      Serial.println("----------------------------------------------------------------------------------");
      #endif

      //Handle potential right click (Previously eligible to right click and this is the later touch to end)
      if (clickEligible == CLICK_RIGHT_PENDING && duration1 < MAX_TAPTOCLICK_TIME)
      {
        clickEligible=CLICK_RIGHT_READY;
        
        #ifdef __CLICK_DEBUG
        Serial.println("Right click ready to be done");
        #endif
      }
      
      //The following is for debug messages
      #ifdef __DEBUG

      Serial.print("Touches ended; lasted ");
      Serial.print(duration1);
      Serial.print(" and ");
      Serial.print(duration2);
      Serial.println(" ms respectively");
      #endif
    }
    else
    {
      //A touch ended and another is still active, which means a multitouch ended; so print a delimiter for multitouch debug
      #ifdef __MULTITOUCH_DEBUG
      if (nTouches)
        Serial.println("----------------------------------------------------------------------------------");
      #endif
      
      //A touch ended; determine which one and track its duration
      if (nTouches ? evenFingerOnly : !lastEvenFingerOnly)
      {
        duration1=millis()-startTime1;
        startX1=-1;
        startY1=-1;
        x1=-1;
        y1=-1;

        //Handle potential right click (Previously eligible to right click and this is the later touch to end)
        if (clickEligible == CLICK_RIGHT_PENDING && !nTouches && duration1 < MAX_TAPTOCLICK_TIME)
        {
          clickEligible=CLICK_RIGHT_READY;

          #ifdef __CLICK_DEBUG
          Serial.println("Right click ready to be done");
          #endif
        }

        //Handle basic left click
        else if (clickEligible == CLICK_LEFT_PENDING && !nTouches && duration1 < MAX_TAPTOCLICK_TIME)
        {
          clickEligible=CLICK_LEFT_READY;

          #ifdef __CLICK_DEBUG
          Serial.println("Left click ready to be done");
          #endif
        }
        
        //The following is for debug messages
        #ifdef __DEBUG

        Serial.print("Touch 1 ended; lasted ");
        Serial.print(duration1);
        Serial.println("ms");
        #endif
      }
      else
      {
        duration2=millis()-startTime2;
        startX2=-1;
        startY2=-1;
        x2=-1;
        y2=-1;

        //Handle potential right click (Previously eligible to right click and this is the later touch to end)
        if (clickEligible == CLICK_RIGHT_PENDING && !nTouches && duration2 < MAX_TAPTOCLICK_TIME)
        {
          clickEligible=CLICK_RIGHT_READY;
          
          #ifdef __CLICK_DEBUG
          Serial.println("Right click ready to be done");
          #endif
        }

        //The following is for debug messages
        #ifdef __DEBUG

        Serial.print("Touch 2 ended; lasted ");
        Serial.print(duration2);
        Serial.println("ms");
        #endif
      }
    }
    //The following is for debug messages
    #ifdef __DEBUG

    Serial.print("Touches ");
    Serial.print(lastNtouches);
    Serial.print(" -> ");
    Serial.println(nTouches);
    #endif
  }

  //Touch(es) started
  else if (nTouches > lastNtouches)
  {
    //A touch started; ignore input for a bit
    lastNtouchChange=millis();
    
    if (lastNtouches == 0 && nTouches == 2)
    {
      //Both touches started; track their start times
      startTime1=millis();
      startTime2=millis();

      //I hate to do this SO much but erratic reporting would prevail otherwise
      //delay(TOUCH_DELAY);
      
      last1x=x1;
      last1y=y1;
      last2x=x2;
      last2y=y2;
      
      /*
      startX1=x1;
      startY1=y1;
      startX2=x2;
      startY2=y2;
      */
      
      //Make these touches eligible to be a right click
      clickEligible=CLICK_RIGHT_PENDING;

      #ifdef __CLICK_DEBUG
      Serial.println("Right click now pending");
      #endif
      
      //The following is for debug messages
      #ifdef __DEBUG

      Serial.println("Both touches started");
      #endif
    }
    else
    {
      if ((!lastNtouches) || lastEvenFingerOnly)
      {
        //Track starting touch time and position
        startTime1=millis();
        last1x=x1;
        last1y=y1;

        //Make this touch eligible to be a left click
        clickEligible=CLICK_LEFT_PENDING;

        #ifdef __CLICK_DEBUG
        Serial.println("Left click now pending");
        #endif
        
        //The following is for debug messages
        #ifdef __DEBUG

        Serial.println("Touch 1 started");
        #endif
      }
      else
      {
        //Track starting touch time and position
        startTime2=millis();
        last2x=x2;
        last2y=y2;
        
        //The following is for debug messages
        #ifdef __DEBUG

        Serial.println("Touch 2 started");
        #endif
      }
    }
    //The following is for debug messages
    #ifdef __DEBUG

    Serial.print("Touches ");
    Serial.print(lastNtouches);
    Serial.print(" -> ");
    Serial.println(nTouches);
    #endif
  }
  
  //Track the relative positions of any active touches
  if (nTouches)
  {
    //Odd numbered finger
    if ((last1x != x1 || last1y != y1) && (nTouches == 2 || !evenFingerOnly))
    {
      //The following is for debug messages
      #ifdef __DEBUG
      Serial.print("Odd finger moved: (");
      Serial.print(last1x);
      Serial.print(", ");
      Serial.print(last1y);
      Serial.print(") -> (");
      Serial.print(x1);
      Serial.print(", ");
      Serial.print(y1);
      Serial.println(")");
      #endif
    }

    //Even numbered finger
    if ((last2x != x2 || last2y != y2) && (nTouches == 2 || evenFingerOnly))
    {
      //The following is for debug messages
      #ifdef __DEBUG
      Serial.print("Even finger moved: (");
      Serial.print(last2x);
      Serial.print(", ");
      Serial.print(last2y);
      Serial.print(") -> (");
      Serial.print(x2);
      Serial.print(", ");
      Serial.print(y2);
      Serial.println(")");
      #endif
    }
  }

  //Handle touchscreen erratic reporting when touches start and end
  if (millis()-lastNtouchChange > TOUCH_DELAY //&& last1x >= 0
    //&& last1y >= 0 && last2x >= 0 && last2y >= 0
    )
  {
    //Handle initial position setting after waiting for values to stabilize
    //YAY I DISCOVERED YET ANOTHER BONEHEADED FT6336U BEHAVIOR CALLED MISREPORTING
    //ALL 1S WHENEVER A TOUCH STARTS SOMETIMES (OK MAYBE NOT BONEHEADED BUT CERTAINLY
    //NOT MENTIONED IN ANY DATASHEET)

    //If the first touch has just started and the resulting time period has just passed
    if (startX1 == -1 && x1 > 0 && x1 != 0x0FFF && y1 != 0x0FFF)
    {
      startX1=x1;
      startY1=y1;
    }

    //If the second touch has just started and the resulting time period has just passed
    if (startX2 == -1 && x2 > 0 && x2 != 0x0FFF && y2 != 0x0FFF)
    {
      startX2=x2;
      startY2=y2;
    }
    
    //Handle mouse moving
    if (nTouches == 1 && !evenFingerOnly)
    {
      //Moved sufficiently; can't be a click anymore (Check if click is active already to avoid messing with clearing clicks)
      if ((x1-startX1)*(x1-startX1)+(y1-startY1)*(y1-startY1) > MAX_TAPTOCLICK_DISPLACEMENT_SQUARE)
      {
        clickEligible=CLICK_IDLE;
        
        #ifdef __CLICK_DEBUG
        Serial.println("No more chance at being a click; touch(es) moved");
        #endif
      }

      //Partial mouse movement to preserve scale
      float xIntermediate=MOVE_SCALE*(last1y-y1), yIntermediate=MOVE_SCALE*(x1-last1x);
      int8_t xScaled=int8_t(xIntermediate), yScaled=int8_t(yIntermediate);
      partialXMove+=xIntermediate-xScaled;
      partialYMove+=yIntermediate-yScaled;
      if (partialXMove >= 1)
      {
        partialXMove--;
        xScaled++;
      }
      else if (partialXMove <= -1)
      {
        partialXMove++;
        xScaled--;
      }
      if (partialYMove >= 1)
      {
        partialYMove--;
        yScaled++;
      }
      else if (partialYMove <= -1)
      {
        partialYMove++;
        yScaled--;
      }
      
      #ifdef __MOUSE_DEBUG

      //If the mouse actually moved
      if (xScaled || yScaled)
      {
        Serial.print("Moving mouse: (");
        Serial.print(xScaled);
        Serial.print(", ");
        Serial.print(yScaled);
        Serial.println(")");
      }
      
      #endif

      //Rule out some unrealistic jumps in mouse movement
      if (xScaled*xScaled+yScaled*yScaled <= MAX_MOUSE_DELTA_R_SQUARE)
        Mouse.move(xScaled, yScaled, 0);
    }
    if (nTouches == 1 && evenFingerOnly) 
    {
      //Moved; can't be a click anymore (Check if click is active already to avoid messing with clearing clicks)
      if ((x2-startX2)*(x2-startX2)+(y2-startY2)*(y2-startY2) > MAX_TAPTOCLICK_DISPLACEMENT_SQUARE)
      {
        clickEligible=CLICK_IDLE;
  
        #ifdef __CLICK_DEBUG
        Serial.println("No more chance at being a click; touch(es) moved");
        #endif
      }
      
      int8_t xScaled=int8_t(MOVE_SCALE*(last2y-y2)), yScaled=int8_t(MOVE_SCALE*(x2-last2x));
      
      #ifdef __MOUSE_DEBUG
      
      //If the mouse actually moved
      if (xScaled || yScaled)
      {
        Serial.print("Moving mouse: (");
        Serial.print(xScaled);
        Serial.print(", ");
        Serial.print(yScaled);
        Serial.println(")");
      }
      
      #endif

      //Rule out some unrealistic jumps in mouse movement
      if (xScaled*xScaled+yScaled*yScaled <= MAX_MOUSE_DELTA_R_SQUARE)
        Mouse.move(xScaled, yScaled, 0);
    }
  
    //Handle scrolling, sidescrolling, and zooming
    if (nTouches == 2)
    {
      #ifdef __MULTITOUCH_DEBUG
      //If anything actually changed since last run of loop()
      if (x1 != last1x || y1 != last1y || x2 != last2x || y2 != last2y)
      {
        //Make all variables to print
        int16_t currentDist=sqrt(((int32_t)(x2-x1))*(x2-x1)+((int32_t)(y2-y1))*(y2-y1)), initDist=sqrt(((int32_t)(startX2-startX1))*(startX2-startX1)+((int32_t)(startY2-startY1))*(startY2-startY1));
        int16_t distDiff=currentDist-initDist;
  
        Serial.print(initDist);
        Serial.print("\t");
        Serial.print(currentDist);
        Serial.print("\t");
        Serial.print(distDiff);
  
        Serial.print("\t");
        
        int16_t avgYStart=(startY1+startY2)/2, avgYCurrent=(y1+y2)/2, avgXStart=(startX1+startX2)/2, avgXCurrent=(x1+x2)/2;
        int16_t avgYDiff=avgYCurrent-avgYStart, avgXDiff=avgXCurrent-avgXStart;
        
        Serial.print(avgYStart);
        Serial.print("\t");
        Serial.print(avgYCurrent);
        Serial.print("\t");
        Serial.print(avgYDiff);
  
        Serial.print("\t");
        
        Serial.print(avgXStart);
        Serial.print("\t");
        Serial.print(avgXCurrent);
        Serial.print("\t");
        Serial.print(avgXDiff);
        
        Serial.println();
      }
      #endif
      
      //Moved more than required distance; can't be a click anymore (Check if click is active already to avoid messing with clearing clicks)
      if (((x1-startX1)*(x1-startX1)+(y1-startY1)*(y1-startY1) > MAX_TAPTOCLICK_DISPLACEMENT_SQUARE || (x2-startX2)*(x2-startX2)+(y2-startY2)*(y2-startY2) > MAX_TAPTOCLICK_DISPLACEMENT_SQUARE) && !multiTouchAction)
      {
        clickEligible=CLICK_IDLE;

        #ifdef __CLICK_DEBUG
        Serial.println("No more chance at being a click; touch(es) moved");
        #endif

        //Now that this 2 finger gesture has exited its bounds to be a right click, determine what type of gesture it should be classed as

        //If the distance between the 2 points changes more than the max tap to click displacement, class this gesture as a zoom

        int16_t distanceDiff=sqrt(((int32_t)(x2-x1))*(x2-x1)+((int32_t)(y2-y1))*(y2-y1))-sqrt(((int32_t)(startX2-startX1))*(startX2-startX1)+((int32_t)(startY2-startY1))*(startY2-startY1));

        //Calculate average point's y movement on small touchscreen (this is X movement in mouse coordinates)
        //and same for x movement on small screen (this is Y movement in mouse coordinates)
        int16_t avgScreenYDiff=((y1+y2)-(startY1+startY2))/2, avgScreenXDiff=((x1+x2)-(startX1+startX2))/2;
        
        if (distanceDiff*distanceDiff > 32*MAX_TAPTOCLICK_DISPLACEMENT_SQUARE)
        {
          //Control + scroll to zoom and set gesture appropriately
          Keyboard.press(KEY_LEFT_CTRL);
          multiTouchAction=MULTI_ZOOM;

          //Debug mouse prints have this surrounding it
          #ifdef __GESTURE_DEBUG
      
          Serial.println("IT'S A ZOOM!!");
          
          #endif
        }
        
        //If the average of the 2 points moved a squared x component (in mouse coordinates) greater than or equal to the max tap to click displacement,
        //it should be classed as a sidescroll (2 fingers, common horizontal direction)
        else if (avgScreenYDiff*avgScreenYDiff > 32*MAX_TAPTOCLICK_DISPLACEMENT_SQUARE)
        {
          //Shift + scroll to sidescroll and set gesture appropriately
          Keyboard.press(KEY_LEFT_SHIFT);
          multiTouchAction=MULTI_SIDESCROLL;

          //Debug mouse prints have this surrounding it
          #ifdef __GESTURE_DEBUG
      
          Serial.println("IT'S A SIDESCROLL!!");
          
          #endif
        }

        //If the average of the 2 points moved a squared y component (in mouse coordinates) greater than or equal to the max tap to click displacement,
        //it should be classed as a scroll (2 fingers, common horizontal direction)
        else if (avgScreenXDiff*avgScreenXDiff > 16*MAX_TAPTOCLICK_DISPLACEMENT_SQUARE)
        {
          //Shift + scroll to sidescroll and set gesture appropriately
          multiTouchAction=MULTI_SCROLL;

          //Debug mouse prints have this surrounding it
          #ifdef __GESTURE_DEBUG
      
          Serial.println("IT'S A SCROLL!!");
          
          #endif
        }
        
        //Debug mouse prints have this surrounding it
        #ifdef __GESTURE_DEBUG
        
        else Serial.println("Somehow undecided?"); 
        
        #endif
      }

      //Keep track of "not quite full" scroll increments for the entire time the double touch is active
      float scrollIntermediate=0;

      //Find out which scroll should be used
      switch (multiTouchAction)
      {
      case MULTI_ZOOM:
        scrollIntermediate=(sqrt(((int32_t)(x2-x1))*(x2-x1)+((int32_t)(y2-y1))*(y2-y1))-sqrt(((int32_t)(last2x-last1x))*(last2x-last1x)+((int32_t)(last2y-last1y))*(last2y-last1y)))/100.0;

        //Keep an eye on zoom; it gets a little crazy sometimes
        #ifdef __GESTURE_DEBUG
        Serial.println(scrollIntermediate);
        #endif
        
        break;
      case MULTI_SIDESCROLL:
        scrollIntermediate=((y1+y2)-(last1y+last2y))/16.0;
        break;
      case MULTI_SCROLL:
        scrollIntermediate=((x1+x2)-(last1x+last2x))/16.0;
        break;
      default:
        break;
      }
      
      int8_t scrollScaled=int8_t(scrollIntermediate);
      partialScroll+=scrollIntermediate-scrollScaled;

      //Handle using previous partial scrolls
      //2 decimals less than 1 added up can never be >= 2, so no loop is needed; only an if statement
      if (partialScroll >= 1)
      {
        partialScroll--;
        scrollScaled++;
      }
      else if (partialScroll <= -1)
      {
        partialScroll++;
        scrollScaled--;
      }
      
      #ifdef __MOUSE_DEBUG
      Serial.print("Scrolling mouse: (");
      Serial.print(scrollScaled);
      Serial.println(")");
      #endif

      //If the multitouch gesture is anything other than undecided
      #ifndef __MULTITOUCH_DEBUG
      if (multiTouchAction)
        Mouse.move(0, 0, scrollScaled);
      #endif
    }
  }

  //Handle presses/releases of hardware buttons
  uint8_t leftBtn=digitalRead(LEFT_BTN), rightBtn=digitalRead(RIGHT_BTN);

  //Tap to click would be redundant when a button is already held (they are active low)
  if (!leftBtn || !rightBtn)
    clickEligible=CLICK_IDLE;

  //If left button changed state
  if (leftBtn != lastLeftBtn)
  {
    //If the state change was a press (active low)
    if (!leftBtn)
    {
      #ifdef __BUTTON_DEBUG
      Serial.println("Left button pressed");
      #endif

      //Actually press the button for the HID compliant mouse driver to see
      Mouse.press(MOUSE_LEFT);
    }

    //Otherwise, it was a release
    else
    {
      #ifdef __BUTTON_DEBUG
      Serial.println("Left button released");
      #endif

      //Actually release the button for the HID compliant mouse driver to see
      Mouse.release(MOUSE_LEFT);
    }
  }

  //If right button changed state
  if (rightBtn != lastRightBtn)
  {
    //If the state change was a press (active low)
    if (!rightBtn)
    {
      #ifdef __BUTTON_DEBUG
      Serial.println("Right button pressed");
      #endif

      //Actually press the button for the HID compliant mouse driver to see
      Mouse.press(MOUSE_RIGHT);
    }

    //Otherwise, it was a release
    else
    {
      #ifdef __BUTTON_DEBUG
      Serial.println("Right button released");
      #endif

      //Actually release the button for the HID compliant mouse driver to see
      Mouse.release(MOUSE_RIGHT);
    }
  }

  lastLeftBtn=leftBtn;
  lastRightBtn=rightBtn;
  
  //Handle tap to click (Only do if clickEligible is correctly set)
  if (clickEligible == CLICK_LEFT_READY || clickEligible == CLICK_RIGHT_READY)
  {
    #ifdef __CLICK_DEBUG
    Serial.println("About to start click");
    #endif
    
    //Left click flag requested
    if (clickEligible == CLICK_LEFT_READY)
    {
      Mouse.click(MOUSE_LEFT);

      #ifdef __CLICK_DEBUG
      Serial.print("Left");
      #endif
    }

    //Right click flag requested
    if (clickEligible == CLICK_RIGHT_READY)
    {
      Mouse.click(MOUSE_RIGHT);

      #ifdef __CLICK_DEBUG
      Serial.print("Right");
      #endif
    }

    //Clear click flag
    clickEligible=CLICK_IDLE;
    
    #ifdef __CLICK_DEBUG
    Serial.println(" clicked");
    #endif
  }

  //Next we track screen dim timer (Refresh it if any touches are active or started)
  if (nTouches)
  {
    offTimer=millis();
    analogWrite(TFT_PWM, BR_TOUCH);
  }

  //Display brightness timeout expired
  if (millis()-offTimer > TIMEOUT_TOUCH)
  {
    analogWrite(TFT_PWM, BR_IDLE);
  }

  //LAST we store all current touch data into previous variables
  lastNtouches=nTouches;
  lastEvenFingerOnly=evenFingerOnly;
  last1x=x1;
  last1y=y1;
  last2x=x2;
  last2y=y2;
}
