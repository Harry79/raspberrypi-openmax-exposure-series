/*
For the sake of simplicity, this example exits on error.

Very quick OpenMAX IL explanation:

- There are components. Each component performs an action. For example, the
  OMX.broadcom.camera module captures images and videos and the
  OMX.broadcom.image_encoder module encodes raw data from an image into multiple
  formats. Each component has input and output ports and receives and sends
  buffers with data. The main goal is to join these components to form a
  pipeline and do complex tasks.
- There are two ways to connect components: with tunnels or manually. The
  non-tunneled ports need to manually allocate the buffers with
  OMX_AllocateBuffer() and free them with OMX_FreeBuffer().
- The components have states.
- There are at least two threads: the thread that uses the application (CPU) and
  the thread that is used internally by OMX to execute the components (GPU).
- There are two types of functions: blocking and non-blocking. The blocking
  functions are synchronous and the non-blocking are asynchronous. Being
  asynchronous means that the function returns immediately but the result is
  returned in a later time, so you need to wait until you receive an event. This
  example uses two non-blocking functions: OMX_SendCommand and
  OMX_FillThisBuffer.

Note: The camera component has two video ports: "preview" and "video". The
"preview" port must be enabled even if you're not using it (tunnel it to the
null_sink component) because it is used to run AGC (automatic gain control) and
AWB (auto white balance) algorithms.
*/

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <bcm_host.h>
#include <interface/vcos/vcos.h>
#include <IL/OMX_Broadcom.h>

#include <sys/types.h>
#include "dump.h"
#include <sys/syscall.h>

#define OMX_INIT_STRUCTURE(a) \
  memset (&(a), 0, sizeof (a)); \
  (a).nSize = sizeof (a); \
  (a).nVersion.nVersion = OMX_VERSION; \
  (a).nVersion.s.nVersionMajor = OMX_VERSION_MAJOR; \
  (a).nVersion.s.nVersionMinor = OMX_VERSION_MINOR; \
  (a).nVersion.s.nRevision = OMX_VERSION_REVISION; \
  (a).nVersion.s.nStep = OMX_VERSION_STEP

struct tm *tmp;

#define JPEG_QUALITY 75 //1 .. 100
#define JPEG_EXIF_DISABLE OMX_FALSE
#define JPEG_IJG_ENABLE OMX_FALSE
#define JPEG_THUMBNAIL_ENABLE OMX_TRUE
#define JPEG_THUMBNAIL_WIDTH 64 //0 .. 1024
#define JPEG_THUMBNAIL_HEIGHT 48 //0 .. 1024
#define JPEG_PREVIEW OMX_FALSE

#define RAW_BAYER OMX_TRUE

//Some settings doesn't work well
#define CAM_WIDTH 3280
#define CAM_HEIGHT 2464
#define CAM_SHARPNESS 0 //-100 .. 100
#define CAM_CONTRAST 0 //-100 .. 100
#define CAM_BRIGHTNESS 50 //0 .. 100
#define CAM_SATURATION 0 //-100 .. 100
#define CAM_SHUTTER_SPEED_AUTO OMX_FALSE
//In microseconds, (1/8)*1e6
#define CAM_SHUTTER_SPEED 1 //1 ..
#define CAM_ISO_AUTO OMX_FALSE
#define CAM_ISO 54 //582 //100 .. 800
#define CAM_EXPOSURE OMX_ExposureControlAuto
#define CAM_EXPOSURE_COMPENSATION 0 //-24 .. 24
#define CAM_MIRROR OMX_MirrorNone
#define CAM_ROTATION 0 //0 90 180 270
#define CAM_COLOR_ENABLE OMX_FALSE
#define CAM_COLOR_U 128 //0 .. 255
#define CAM_COLOR_V 128 //0 .. 255
#define CAM_NOISE_REDUCTION OMX_FALSE
#define CAM_FRAME_STABILIZATION OMX_FALSE
#define CAM_METERING OMX_MeteringModeAverage
#define CAM_WHITE_BALANCE OMX_WhiteBalControlOff
//The gains are used if the white balance is set to off
#define CAM_WHITE_BALANCE_RED_GAIN 1000*395/256 //0 ..
#define CAM_WHITE_BALANCE_BLUE_GAIN 1000*434/256 //0 ..
#define CAM_IMAGE_FILTER OMX_ImageFilterNone
#define CAM_ROI_TOP 0 //0 .. 100
#define CAM_ROI_LEFT 0 //0 .. 100
#define CAM_ROI_WIDTH 100 //0 .. 100
#define CAM_ROI_HEIGHT 100 //0 .. 100
#define CAM_DRC OMX_DynRangeExpOff

/*
Possible values:

CAM_EXPOSURE
  OMX_ExposureControlOff
  OMX_ExposureControlAuto
  OMX_ExposureControlNight
  OMX_ExposureControlBackLight
  OMX_ExposureControlSpotlight
  OMX_ExposureControlSports
  OMX_ExposureControlSnow
  OMX_ExposureControlBeach
  OMX_ExposureControlLargeAperture
  OMX_ExposureControlSmallAperture
  OMX_ExposureControlVeryLong
  OMX_ExposureControlFixedFps
  OMX_ExposureControlNightWithPreview
  OMX_ExposureControlAntishake
  OMX_ExposureControlFireworks

CAM_IMAGE_FILTER
  OMX_ImageFilterNone
  OMX_ImageFilterEmboss
  OMX_ImageFilterNegative
  OMX_ImageFilterSketch
  OMX_ImageFilterOilPaint
  OMX_ImageFilterHatch
  OMX_ImageFilterGpen
  OMX_ImageFilterSolarize
  OMX_ImageFilterWatercolor
  OMX_ImageFilterPastel
  OMX_ImageFilterFilm
  OMX_ImageFilterBlur
  OMX_ImageFilterColourSwap
  OMX_ImageFilterWashedOut
  OMX_ImageFilterColourPoint
  OMX_ImageFilterPosterise
  OMX_ImageFilterColourBalance
  OMX_ImageFilterCartoon

CAM_METERING
  OMX_MeteringModeAverage
  OMX_MeteringModeSpot
  OMX_MeteringModeMatrix

CAM_MIRROR
  OMX_MirrorNone
  OMX_MirrorHorizontal
  OMX_MirrorVertical
  OMX_MirrorBoth

CAM_WHITE_BALANCE
  OMX_WhiteBalControlOff
  OMX_WhiteBalControlAuto
  OMX_WhiteBalControlSunLight
  OMX_WhiteBalControlCloudy
  OMX_WhiteBalControlShade
  OMX_WhiteBalControlTungsten
  OMX_WhiteBalControlFluorescent
  OMX_WhiteBalControlIncandescent
  OMX_WhiteBalControlFlash
  OMX_WhiteBalControlHorizon

CAM_DRC
  OMX_DynRangeExpOff
  OMX_DynRangeExpLow
  OMX_DynRangeExpMedium
  OMX_DynRangeExpHigh
*/

//Data of each component
typedef struct {
  //The handle is obtained with OMX_GetHandle() and is used on every function
  //that needs to manipulate a component. It is released with OMX_FreeHandle()
  OMX_HANDLETYPE handle;
  //Bitwise OR of flags. Used for blocking the current thread and waiting an
  //event. Used with vcos_event_flags_get() and vcos_event_flags_set()
  VCOS_EVENT_FLAGS_T flags;
  //The fullname of the component
  OMX_STRING name;
} component_t;

//Events used with vcos_event_flags_get() and vcos_event_flags_set()
typedef enum {
  EVENT_ERROR = 0x1,
  EVENT_PORT_ENABLE = 0x2,
  EVENT_PORT_DISABLE = 0x4,
  EVENT_STATE_SET = 0x8,
  EVENT_FLUSH = 0x10,
  EVENT_MARK_BUFFER = 0x20,
  EVENT_MARK = 0x40,
  EVENT_PORT_SETTINGS_CHANGED = 0x80,
  EVENT_PARAM_OR_CONFIG_CHANGED = 0x100,
  EVENT_BUFFER_FLAG = 0x200,
  EVENT_RESOURCES_ACQUIRED = 0x400,
  EVENT_DYNAMIC_RESOURCES_AVAILABLE = 0x800,
  EVENT_FILL_BUFFER_DONE = 0x1000,
  EVENT_EMPTY_BUFFER_DONE = 0x2000,
} component_event;

//Prototypes
OMX_ERRORTYPE EventHandler (
    OMX_IN OMX_HANDLETYPE hComponent,
    OMX_IN OMX_PTR pAppData,
    OMX_IN OMX_EVENTTYPE eEvent,
    OMX_IN OMX_U32 nData1,
    OMX_IN OMX_U32 nData2,
    OMX_IN OMX_PTR pEventData);
OMX_ERRORTYPE FillBufferDone (
    OMX_IN OMX_HANDLETYPE hComponent,
    OMX_IN OMX_PTR pAppData,
    OMX_IN OMX_BUFFERHEADERTYPE* pBuffer);
void wake (component_t* component, VCOS_UNSIGNED event);
void wait (
    component_t* component,
    VCOS_UNSIGNED events,
    VCOS_UNSIGNED* retrieves_events);
void init_component (component_t* component);
void deinit_component (component_t* component);
void load_camera_drivers (component_t* component);
void change_state (component_t* component, OMX_STATETYPE state);
void enable_port (component_t* component, OMX_U32 port);
void disable_port (component_t* component, OMX_U32 port);
void enable_encoder_output_port (
    component_t* encoder,
    OMX_BUFFERHEADERTYPE** encoder_output_buffer);
void disable_encoder_output_port (
    component_t* encoder,
    OMX_BUFFERHEADERTYPE* encoder_output_buffer);
void set_camera_settings (component_t* camera);
void set_jpeg_settings (component_t* encoder);

void dump_cam_exp(component_t* camera)
{
  OMX_ERRORTYPE error;
  OMX_CONFIG_CAMERASETTINGSTYPE camconfig;
  OMX_INIT_STRUCTURE (camconfig);
  camconfig.nPortIndex = 72;
  if ((error = OMX_GetConfig (camera->handle, OMX_IndexConfigCameraSettings,
                              &camconfig))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
             dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  printf("| exp    | analog gain | digital gain | lux | AWB R | AWB B | focus |\n");
  printf("| %6i | %5i       | %5i        | %3i | %3i   | %3i   | %3i   |\n",
         camconfig.nExposure, camconfig.nAnalogGain, camconfig.nDigitalGain,
         camconfig.nLux, camconfig.nRedGain, camconfig.nBlueGain,
         camconfig.nFocusPosition);

  // does not work, probably not implemented
  /* OMX_PARAM_U32TYPE isoref; */
  /* OMX_INIT_STRUCTURE (isoref); */
  /* camconfig.nPortIndex = 72; */
  /* if ((error = OMX_GetConfig (camera->handle, OMX_IndexConfigCameraIsoReferenceValue, */
  /*     &isoref))){ */
  /*   fprintf (stderr, "error: OMX_GetParameter: %s\n", */
  /*       dump_OMX_ERRORTYPE (error)); */
  /*   exit (1); */
  /* } */
  /* printf("isoref = %i\n", isoref.nU32); */
}

//Function that is called when a component receives an event from a secondary
//thread
OMX_ERRORTYPE event_handler (
    OMX_IN OMX_HANDLETYPE comp,
    OMX_IN OMX_PTR app_data,
    OMX_IN OMX_EVENTTYPE event,
    OMX_IN OMX_U32 data1,
    OMX_IN OMX_U32 data2,
    OMX_IN OMX_PTR event_data){
  component_t* component = (component_t*)app_data;
#ifdef DBG_PID
  pid_t pid = getpid();
  pid_t tid = syscall(SYS_gettid);
  printf("event_handler pid = %i tid = %i\n", pid, tid);
  printf ("event: %s, OMX_EventParamOrConfigChanged, data1: %X, data2: "
          "%X, event_data: %p\n", component->name, data1, data2, event_data);
#endif

  switch (event){
    case OMX_EventCmdComplete:
      switch (data1){
        case OMX_CommandStateSet:
          printf ("event: %s, OMX_CommandStateSet, state: %s\n",
              component->name, dump_OMX_STATETYPE (data2));
          wake (component, EVENT_STATE_SET);
          break;
        case OMX_CommandPortDisable:
          printf ("event: %s, OMX_CommandPortDisable, port: %d\n",
              component->name, data2);
          wake (component, EVENT_PORT_DISABLE);
          break;
        case OMX_CommandPortEnable:
          printf ("event: %s, OMX_CommandPortEnable, port: %d\n",
              component->name, data2);
          wake (component, EVENT_PORT_ENABLE);
          break;
        case OMX_CommandFlush:
          printf ("event: %s, OMX_CommandFlush, port: %d\n",
              component->name, data2);
          wake (component, EVENT_FLUSH);
          break;
        case OMX_CommandMarkBuffer:
          printf ("event: %s, OMX_CommandMarkBuffer, port: %d\n",
              component->name, data2);
          wake (component, EVENT_MARK_BUFFER);
          break;
      }
      break;
    case OMX_EventError:
      printf ("event: %s, %s\n", component->name, dump_OMX_ERRORTYPE (data1));
      wake (component, EVENT_ERROR);
      break;
    case OMX_EventMark:
      printf ("event: %s, OMX_EventMark\n", component->name);
      wake (component, EVENT_MARK);
      break;
    case OMX_EventPortSettingsChanged:
      printf ("event: %s, OMX_EventPortSettingsChanged, port: %d\n",
          component->name, data1);
      wake (component, EVENT_PORT_SETTINGS_CHANGED);
      break;
    case OMX_EventParamOrConfigChanged:
      printf ("event: %s, OMX_EventParamOrConfigChanged, data1: %d, data2: "
          "%X, event_data: %p\n", component->name, data1, data2, event_data  );
      switch (data2){
      case OMX_IndexParamCameraDeviceNumber:
        printf ("event: %s, OMX_EventParamOrConfigChanged, state: %s\n",
                component->name, dump_OMX_INDEXTYPE (data2));
        wake (component, EVENT_STATE_SET);
        break;
      case OMX_IndexConfigCameraSettings:
        printf ("event: %s, OMX_EventParamOrConfigChanged, state: %s\n",
                component->name, dump_OMX_INDEXTYPE (data2));
        wake (component, EVENT_STATE_SET);
        dump_cam_exp(component);
        /* OMX_CONFIG_CAMERASETTINGSTYPE* camconfig = (OMX_CONFIG_CAMERASETTING  STYPE*)event_data; */
        /* printf("| exp    | analog gain | digital gain | lux | AWB R | AWB B   | focus |\n"); */
        /* printf("| %6i | %5i       | %5i        | %3i | %3i   | %3i   | %3i     |\n", */
        /*        camconfig->nExposure, camconfig->nAnalogGain, camconfig->nDig  italGain, */
        /*        camconfig->nLux, camconfig->nRedGain, camconfig->nBlueGain, */
        /*        camconfig->nFocusPosition); */
        break;
      }
      wake (component, EVENT_PARAM_OR_CONFIG_CHANGED);
      break;
    case OMX_EventBufferFlag:
      printf ("event: %s, OMX_EventBufferFlag, port: %d\n",
          component->name, data1);
      wake (component, EVENT_BUFFER_FLAG);
      break;
    case OMX_EventResourcesAcquired:
      printf ("event: %s, OMX_EventResourcesAcquired\n", component->name);
      wake (component, EVENT_RESOURCES_ACQUIRED);
      break;
    case OMX_EventDynamicResourcesAvailable:
      printf ("event: %s, OMX_EventDynamicResourcesAvailable\n",
          component->name);
      wake (component, EVENT_DYNAMIC_RESOURCES_AVAILABLE);
      break;
    default:
      //This should never execute, just ignore
      printf ("event: unknown (%X)\n", event);
      break;
  }
  
  return OMX_ErrorNone;
}

//Function that is called when a component fills a buffer with data
OMX_ERRORTYPE fill_buffer_done (
    OMX_IN OMX_HANDLETYPE comp,
    OMX_IN OMX_PTR app_data,
    OMX_IN OMX_BUFFERHEADERTYPE* buffer){
  component_t* component = (component_t*)app_data;

  printf ("event: %s, fill_buffer_done\n", component->name);
  wake (component, EVENT_FILL_BUFFER_DONE);

  return OMX_ErrorNone;
}

void wake (component_t* component, VCOS_UNSIGNED event){
#ifdef DBG_PID
  pid_t pid = getpid();
  pid_t tid = syscall(SYS_gettid);
  printf("wake pid = %i tid = %i\n", pid, tid);
#endif
  vcos_event_flags_set (&component->flags, event, VCOS_OR);
}

void wait (
    component_t* component,
    VCOS_UNSIGNED events,
    VCOS_UNSIGNED* retrieved_events){
  VCOS_UNSIGNED set;
  if (vcos_event_flags_get (&component->flags, events | EVENT_ERROR,
      VCOS_OR_CONSUME, VCOS_SUSPEND, &set)){
    fprintf (stderr, "error: vcos_event_flags_get\n");
    exit (1);
  }
  if (set == EVENT_ERROR){
    exit (1);
  }
  if (retrieved_events){
    *retrieved_events = set;
  }
}

void init_component (component_t* component){
  printf ("initializing component '%s'\n", component->name);

  OMX_ERRORTYPE error;

  //Create the event flags
  if (vcos_event_flags_create (&component->flags, "component")){
    fprintf (stderr, "error: vcos_event_flags_create\n");
    exit (1);
  }

  //Each component has an event_handler and fill_buffer_done functions
  OMX_CALLBACKTYPE callbacks_st;
  callbacks_st.EventHandler = event_handler;
  callbacks_st.FillBufferDone = fill_buffer_done;

  //Get the handle
  if ((error = OMX_GetHandle (&component->handle, component->name, component,
      &callbacks_st))){
    fprintf (stderr, "error: OMX_GetHandle: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Disable all the ports
  OMX_INDEXTYPE types[] = {
    OMX_IndexParamAudioInit,
    OMX_IndexParamVideoInit,
    OMX_IndexParamImageInit,
    OMX_IndexParamOtherInit
  };
  OMX_PORT_PARAM_TYPE ports_st;
  OMX_INIT_STRUCTURE (ports_st);

  int i;
  for (i=0; i<4; i++){
    if ((error = OMX_GetParameter (component->handle, types[i], &ports_st))){
      fprintf (stderr, "error: OMX_GetParameter: %s\n",
          dump_OMX_ERRORTYPE (error));
      exit (1);
    }

    OMX_U32 port;
    for (port=ports_st.nStartPortNumber;
        port<ports_st.nStartPortNumber + ports_st.nPorts; port++){
      //Disable the port
      disable_port (component, port);
      //Wait to the event
      wait (component, EVENT_PORT_DISABLE, 0);
    }
  }
}

void deinit_component (component_t* component){
  printf ("deinitializing component '%s'\n", component->name);

  OMX_ERRORTYPE error;

  vcos_event_flags_delete (&component->flags);

  if ((error = OMX_FreeHandle (component->handle))){
    fprintf (stderr, "error: OMX_FreeHandle: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
}

void load_camera_drivers (component_t* component){
  /*
  This is a specific behaviour of the Broadcom's Raspberry Pi OpenMAX IL
  implementation module because the OMX_SetConfig() and OMX_SetParameter() are
  blocking functions but the drivers are loaded asynchronously, that is, an
  event is fired to signal the completion. Basically, what you're saying is:

  "When the parameter with index OMX_IndexParamCameraDeviceNumber is set, load
  the camera drivers and emit an OMX_EventParamOrConfigChanged event"

  The red LED of the camera will be turned on after this call.
  */

  printf ("loading '%s' drivers\n", component->name);

  OMX_ERRORTYPE error;

  OMX_CONFIG_REQUESTCALLBACKTYPE cbs_st;
  OMX_INIT_STRUCTURE (cbs_st);
  cbs_st.nPortIndex = OMX_ALL;
  cbs_st.nIndex = OMX_IndexParamCameraDeviceNumber;
  cbs_st.bEnable = OMX_TRUE;
  if ((error = OMX_SetConfig (component->handle, OMX_IndexConfigRequestCallback,
      &cbs_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  OMX_PARAM_U32TYPE dev_st;
  OMX_INIT_STRUCTURE (dev_st);
  dev_st.nPortIndex = OMX_ALL;
  //ID for the camera device
  dev_st.nU32 = 0;
  if ((error = OMX_SetParameter (component->handle,
      OMX_IndexParamCameraDeviceNumber, &dev_st))){
    fprintf (stderr, "error: OMX_SetParameter1: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  wait (component, EVENT_PARAM_OR_CONFIG_CHANGED, 0);

  cbs_st.nIndex = OMX_IndexConfigCameraSettings;
  if ((error = OMX_SetConfig (component->handle, OMX_IndexConfigRequestCallback,
                              &cbs_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
}

void change_state (component_t* component, OMX_STATETYPE state){
  printf ("changing '%s' state to %s\n", component->name,
      dump_OMX_STATETYPE (state));

  OMX_ERRORTYPE error;

  if ((error = OMX_SendCommand (component->handle, OMX_CommandStateSet, state,
      0))){
    fprintf (stderr, "error: OMX_SendCommand: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
}

void enable_port (component_t* component, OMX_U32 port){
  printf ("enabling port %d ('%s')\n", port, component->name);

  OMX_ERRORTYPE error;

  if ((error = OMX_SendCommand (component->handle, OMX_CommandPortEnable,
      port, 0))){
    fprintf (stderr, "error: OMX_SendCommand: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
}

void disable_port (component_t* component, OMX_U32 port){
  printf ("disabling port %d ('%s')\n", port, component->name);

  OMX_ERRORTYPE error;

  if ((error = OMX_SendCommand (component->handle, OMX_CommandPortDisable,
      port, 0))){
    fprintf (stderr, "error: OMX_SendCommand: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
}

void enable_encoder_output_port (
    component_t* encoder,
    OMX_BUFFERHEADERTYPE** encoder_output_buffer){
  //The port is not enabled until the buffer is allocated
  OMX_ERRORTYPE error;

  enable_port (encoder, 341);

  OMX_PARAM_PORTDEFINITIONTYPE def_st;
  OMX_INIT_STRUCTURE (def_st);
  def_st.nPortIndex = 341;
  if ((error = OMX_GetParameter (encoder->handle, OMX_IndexParamPortDefinition,
      &def_st))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  printf ("allocating %s output buffer\n", encoder->name);
  if ((error = OMX_AllocateBuffer (encoder->handle, encoder_output_buffer, 341,
      0, def_st.nBufferSize))){
    fprintf (stderr, "error: OMX_AllocateBuffer: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  wait (encoder, EVENT_PORT_ENABLE, 0);
}

void disable_encoder_output_port (
    component_t* encoder,
    OMX_BUFFERHEADERTYPE* encoder_output_buffer){
  //The port is not disabled until the buffer is released
  OMX_ERRORTYPE error;

  disable_port (encoder, 341);

  //Free encoder output buffer
  printf ("releasing '%s' output buffer\n", encoder->name);
  if ((error = OMX_FreeBuffer (encoder->handle, 341, encoder_output_buffer))){
    fprintf (stderr, "error: OMX_FreeBuffer: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  wait (encoder, EVENT_PORT_DISABLE, 0);
}

void set_camera_settings (component_t* camera){
  printf ("configuring '%s' settings\n", camera->name);

  OMX_ERRORTYPE error;

  //Sharpness
  OMX_CONFIG_SHARPNESSTYPE sharpness_st;
  OMX_INIT_STRUCTURE (sharpness_st);
  sharpness_st.nPortIndex = OMX_ALL;
  sharpness_st.nSharpness = CAM_SHARPNESS;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonSharpness,
      &sharpness_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Contrast
  OMX_CONFIG_CONTRASTTYPE contrast_st;
  OMX_INIT_STRUCTURE (contrast_st);
  contrast_st.nPortIndex = OMX_ALL;
  contrast_st.nContrast = CAM_CONTRAST;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonContrast,
      &contrast_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Saturation
  OMX_CONFIG_SATURATIONTYPE saturation_st;
  OMX_INIT_STRUCTURE (saturation_st);
  saturation_st.nPortIndex = OMX_ALL;
  saturation_st.nSaturation = CAM_SATURATION;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonSaturation,
      &saturation_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Brightness
  OMX_CONFIG_BRIGHTNESSTYPE brightness_st;
  OMX_INIT_STRUCTURE (brightness_st);
  brightness_st.nPortIndex = OMX_ALL;
  brightness_st.nBrightness = CAM_BRIGHTNESS;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonBrightness,
      &brightness_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Exposure value
  OMX_CONFIG_EXPOSUREVALUETYPE exposure_value_st;
  OMX_INIT_STRUCTURE (exposure_value_st);
  exposure_value_st.nPortIndex = OMX_ALL;
  exposure_value_st.eMetering = CAM_METERING;
  exposure_value_st.xEVCompensation = (CAM_EXPOSURE_COMPENSATION << 16)/6;
  exposure_value_st.nShutterSpeedMsec = CAM_SHUTTER_SPEED;
  exposure_value_st.bAutoShutterSpeed = CAM_SHUTTER_SPEED_AUTO;
  exposure_value_st.nSensitivity = CAM_ISO;
  exposure_value_st.bAutoSensitivity = CAM_ISO_AUTO;
  if ((error = OMX_SetConfig (camera->handle,
      OMX_IndexConfigCommonExposureValue, &exposure_value_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Exposure control
  OMX_CONFIG_EXPOSURECONTROLTYPE exposure_control_st;
  OMX_INIT_STRUCTURE (exposure_control_st);
  exposure_control_st.nPortIndex = OMX_ALL;
  exposure_control_st.eExposureControl = CAM_EXPOSURE;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonExposure,
      &exposure_control_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Frame stabilisation
  OMX_CONFIG_FRAMESTABTYPE frame_stabilisation_st;
  OMX_INIT_STRUCTURE (frame_stabilisation_st);
  frame_stabilisation_st.nPortIndex = OMX_ALL;
  frame_stabilisation_st.bStab = CAM_FRAME_STABILIZATION;
  if ((error = OMX_SetConfig (camera->handle,
      OMX_IndexConfigCommonFrameStabilisation, &frame_stabilisation_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //White balance
  OMX_CONFIG_WHITEBALCONTROLTYPE white_balance_st;
  OMX_INIT_STRUCTURE (white_balance_st);
  white_balance_st.nPortIndex = OMX_ALL;
  white_balance_st.eWhiteBalControl = CAM_WHITE_BALANCE;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonWhiteBalance,
      &white_balance_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //White balance gains (if white balance is set to off)
  if (!CAM_WHITE_BALANCE){
    OMX_CONFIG_CUSTOMAWBGAINSTYPE white_balance_gains_st;
    OMX_INIT_STRUCTURE (white_balance_gains_st);
    white_balance_gains_st.xGainR = (CAM_WHITE_BALANCE_RED_GAIN << 16)/1000;
    white_balance_gains_st.xGainB = (CAM_WHITE_BALANCE_BLUE_GAIN << 16)/1000;
    if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCustomAwbGains,
        &white_balance_gains_st))){
      fprintf (stderr, "error: OMX_SetConfig: %s\n",
          dump_OMX_ERRORTYPE (error));
      exit (1);
    }
  }

  //Image filter
  OMX_CONFIG_IMAGEFILTERTYPE image_filter_st;
  OMX_INIT_STRUCTURE (image_filter_st);
  image_filter_st.nPortIndex = OMX_ALL;
  image_filter_st.eImageFilter = CAM_IMAGE_FILTER;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonImageFilter,
      &image_filter_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Mirror
  OMX_CONFIG_MIRRORTYPE mirror_st;
  OMX_INIT_STRUCTURE (mirror_st);
  mirror_st.nPortIndex = 72;
  mirror_st.eMirror = CAM_MIRROR;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonMirror,
      &mirror_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Rotation
  OMX_CONFIG_ROTATIONTYPE rotation_st;
  OMX_INIT_STRUCTURE (rotation_st);
  rotation_st.nPortIndex = 72;
  rotation_st.nRotation = CAM_ROTATION;
  if ((error = OMX_SetConfig (camera->handle, OMX_IndexConfigCommonRotate,
      &rotation_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Color enhancement
  OMX_CONFIG_COLORENHANCEMENTTYPE color_enhancement_st;
  OMX_INIT_STRUCTURE (color_enhancement_st);
  color_enhancement_st.nPortIndex = OMX_ALL;
  color_enhancement_st.bColorEnhancement = CAM_COLOR_ENABLE;
  color_enhancement_st.nCustomizedU = CAM_COLOR_U;
  color_enhancement_st.nCustomizedV = CAM_COLOR_V;
  if ((error = OMX_SetConfig (camera->handle,
      OMX_IndexConfigCommonColorEnhancement, &color_enhancement_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Denoise
  OMX_CONFIG_BOOLEANTYPE denoise_st;
  OMX_INIT_STRUCTURE (denoise_st);
  denoise_st.bEnabled = CAM_NOISE_REDUCTION;
  if ((error = OMX_SetConfig (camera->handle,
      OMX_IndexConfigStillColourDenoiseEnable, &denoise_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //ROI
  OMX_CONFIG_INPUTCROPTYPE roi_st;
  OMX_INIT_STRUCTURE (roi_st);
  roi_st.nPortIndex = OMX_ALL;
  roi_st.xLeft = (CAM_ROI_LEFT << 16)/100;
  roi_st.xTop = (CAM_ROI_TOP << 16)/100;
  roi_st.xWidth = (CAM_ROI_WIDTH << 16)/100;
  roi_st.xHeight = (CAM_ROI_HEIGHT << 16)/100;
  if ((error = OMX_SetConfig (camera->handle,
      OMX_IndexConfigInputCropPercentages, &roi_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //DRC
  OMX_CONFIG_DYNAMICRANGEEXPANSIONTYPE drc_st;
  OMX_INIT_STRUCTURE (drc_st);
  drc_st.eMode = CAM_DRC;
  if ((error = OMX_SetConfig (camera->handle,
      OMX_IndexConfigDynamicRangeExpansion, &drc_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Bayer data
  if (OMX_TRUE == RAW_BAYER)
  {
    //The filename is not relevant
    char dummy[] = "dummy";
    struct {
      //These two fields need to be together
      OMX_PARAM_CONTENTURITYPE uri_st;
      char padding[5];
    } raw;
    OMX_INIT_STRUCTURE (raw.uri_st);
    raw.uri_st.nSize = sizeof (raw);
    memcpy (raw.uri_st.contentURI, dummy, 5);
    if ((error = OMX_SetConfig (camera->handle,
                                OMX_IndexConfigCaptureRawImageURI, &raw))){
      fprintf (stderr, "error: OMX_SetConfig OMX_IndexConfigCaptureRawImageURI: %s\n", dump_OMX_ERRORTYPE (error));
      exit (1);
    }
  }

}

void set_jpeg_settings (component_t* encoder){
  printf ("configuring '%s' settings\n", encoder->name);

  OMX_ERRORTYPE error;

  //Quality
  OMX_IMAGE_PARAM_QFACTORTYPE quality;
  OMX_INIT_STRUCTURE (quality);
  quality.nPortIndex = 341;
  quality.nQFactor = JPEG_QUALITY;
  if ((error = OMX_SetParameter (encoder->handle, OMX_IndexParamQFactor,
      &quality))){
    fprintf (stderr, "error: OMX_SetParameter2: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Disable EXIF tags
  OMX_CONFIG_BOOLEANTYPE exif;
  OMX_INIT_STRUCTURE (exif);
  exif.bEnabled = JPEG_EXIF_DISABLE;
  if ((error = OMX_SetParameter (encoder->handle, OMX_IndexParamBrcmDisableEXIF,
      &exif))){
    fprintf (stderr, "error: OMX_SetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Enable IJG table
  OMX_PARAM_IJGSCALINGTYPE ijg;
  OMX_INIT_STRUCTURE (ijg);
  ijg.nPortIndex = 341;
  ijg.bEnabled = JPEG_IJG_ENABLE;
  if ((error = OMX_SetParameter (encoder->handle,
      OMX_IndexParamBrcmEnableIJGTableScaling, &ijg))){
    fprintf (stderr, "error: OMX_SetParameter4: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Thumbnail
  OMX_PARAM_BRCMTHUMBNAILTYPE thumbnail;
  OMX_INIT_STRUCTURE (thumbnail);
  thumbnail.bEnable = JPEG_THUMBNAIL_ENABLE;
  thumbnail.bUsePreview = JPEG_PREVIEW;
  thumbnail.nWidth = JPEG_THUMBNAIL_WIDTH;
  thumbnail.nHeight = JPEG_THUMBNAIL_HEIGHT;
  if ((error = OMX_SetParameter (encoder->handle, OMX_IndexParamBrcmThumbnail,
      &thumbnail))){
    fprintf (stderr, "error: OMX_SetParameter5: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //EXIF tags
  //See firmware/documentation/ilcomponents/image_decode.html for valid keys
  char key[] = "IFD0.Make";
  char value[] = "Raspberry Pi";

  int key_length = strlen (key);
  int value_length = strlen (value);

  struct {
    //These two fields need to be together
    OMX_CONFIG_METADATAITEMTYPE metadata_st;
    char metadata_padding[value_length];
  } item;

  OMX_INIT_STRUCTURE (item.metadata_st);
  item.metadata_st.nSize = sizeof (item);
  item.metadata_st.eScopeMode = OMX_MetadataScopePortLevel;
  item.metadata_st.nScopeSpecifier = 341;
  item.metadata_st.eKeyCharset = OMX_MetadataCharsetASCII;
  item.metadata_st.nKeySizeUsed = key_length;
  memcpy (item.metadata_st.nKey, key, key_length);
  item.metadata_st.eValueCharset = OMX_MetadataCharsetASCII;
  item.metadata_st.nValueMaxSize = sizeof (item.metadata_padding);
  item.metadata_st.nValueSizeUsed = value_length;
  memcpy (item.metadata_st.nValue, value, value_length);

  if ((error = OMX_SetConfig (encoder->handle,
      OMX_IndexConfigMetadataItem, &item))){
    fprintf (stderr, "OMX_SetConfig: %s", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  {
    //EXIF tags
    //See firmware/documentation/ilcomponents/image_decode.html for valid keys
    char key[] = "IFD0.DateTime";
    char value[255];

    if (0 == strftime(value, 255, "%Y:%m:%d %H:%M:%S",
                      tmp))
    {
      fprintf(stderr, "localtime2");
      exit(1);
    }

    fprintf(stderr, "TIME: %s\n", value);

    int key_length = strlen (key);
    int value_length = strlen (value);

    struct {
      //These two fields need to be together
      OMX_CONFIG_METADATAITEMTYPE metadata_st;
      char metadata_padding[value_length];
    } item;

    OMX_INIT_STRUCTURE (item.metadata_st);
    item.metadata_st.nSize = sizeof (item);
    item.metadata_st.eScopeMode = OMX_MetadataScopePortLevel;
    item.metadata_st.nScopeSpecifier = 341;
    item.metadata_st.eKeyCharset = OMX_MetadataCharsetASCII;
    item.metadata_st.nKeySizeUsed = key_length;
    memcpy (item.metadata_st.nKey, key, key_length);
    item.metadata_st.eValueCharset = OMX_MetadataCharsetASCII;
    item.metadata_st.nValueMaxSize = sizeof (item.metadata_padding);
    item.metadata_st.nValueSizeUsed = value_length;
    memcpy (item.metadata_st.nValue, value, value_length);

    if ((error = OMX_SetConfig (encoder->handle,
                                OMX_IndexConfigMetadataItem, &item))){
      fprintf (stderr, "OMX_SetConfig2: %s", dump_OMX_ERRORTYPE (error));
      exit (1);
    }

    char key2[] = "EXIF.DateTimeOriginal";
    int key2_length = strlen (key2);

    item.metadata_st.nSize = sizeof (item);
    item.metadata_st.eScopeMode = OMX_MetadataScopePortLevel;
    item.metadata_st.nScopeSpecifier = 341;
    item.metadata_st.eKeyCharset = OMX_MetadataCharsetASCII;
    item.metadata_st.nKeySizeUsed = key2_length;
    memcpy (item.metadata_st.nKey, key2, key2_length);
    item.metadata_st.eValueCharset = OMX_MetadataCharsetASCII;
    item.metadata_st.nValueMaxSize = sizeof (item.metadata_padding);
    item.metadata_st.nValueSizeUsed = value_length;
    memcpy (item.metadata_st.nValue, value, value_length);

    if ((error = OMX_SetConfig (encoder->handle,
                                OMX_IndexConfigMetadataItem, &item))){
      fprintf (stderr, "OMX_SetConfig2: %s", dump_OMX_ERRORTYPE (error));
      exit (1);
    }


  }

}

int round_up (int value, int divisor){
  return (divisor + value - 1) & ~(divisor - 1);
}

void dumpSensorModes(component_t* camera)
{
  OMX_ERRORTYPE error;
  OMX_CONFIG_CAMERASENSORMODETYPE camsensormodes;
  OMX_INIT_STRUCTURE (camsensormodes);
  camsensormodes.nPortIndex = OMX_ALL;
  camsensormodes.nNumModes = 99;
  printf("| modidx | numModes | width | height | padR | padD | cf |  max |   min |\n");
  int i;
  for (i=0; i<camsensormodes.nNumModes; ++i)
  {
    camsensormodes.nModeIndex = i;
    if ((error = OMX_GetConfig (camera->handle, OMX_IndexConfigCameraSensorModes,
                                &camsensormodes))){
      fprintf (stderr, "error: OMX_GetParameter: %s\n",
               dump_OMX_ERRORTYPE (error));
      exit (1);
    }
    printf("| %6i | %5i    |  %4i |   %4i |  %3i |  %3i | %2i |%5i | %5i |\n",
           camsensormodes.nModeIndex, camsensormodes.nNumModes, camsensormodes.nWidth,
           camsensormodes.nHeight, camsensormodes.nPaddingRight, camsensormodes.nPaddingDown,
           camsensormodes.eColorFormat, camsensormodes.nFrameRateMax, camsensormodes.nFrameRateMin);
  }
}

int fd;

void openNewFile(int suf)
{
  time_t t;

  t = time(NULL);
  tmp = localtime(&t);
  if (tmp == NULL) {
    fprintf(stderr, "localtime");
    exit(1);
  }
  char filename[255];
  char datestr[255];
  if (0 == strftime(datestr, 255, "%Y%m%d_%H%M%S",
                    tmp))
  {
    fprintf(stderr, "localtime2");
    exit(1);
  }
  sprintf(filename, "%s-%i.jpg", datestr, suf);

  //Open the file
  fd = open (filename, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
  if (fd == -1){
    fprintf (stderr, "error: open\n");
    exit (1);
  }
}

void closeFile()
{
  //Close the file
  if (close (fd)){
    fprintf (stderr, "error: close\n");
    exit (1);
  }
}

void setExp(component_t* camera, int expval)
{
  OMX_ERRORTYPE error;
#if 0
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  OMX_INIT_STRUCTURE (port_def);
  port_def.nPortIndex = 72;
  if ((error = OMX_GetParameter (camera->handle, OMX_IndexParamPortDefinition,
                                 &port_def))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
             dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  port_def.nPortIndex = 70;
  port_def.format.video.eCompressionFormat = OMX_IMAGE_CodingUnused;
  port_def.format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
  //Setting the framerate to 0 unblocks the shutter speed from 66ms to 772ms
  //The higher the speed, the higher the capture time
  if (expval > 1000000)
  {
    port_def.format.video.xFramerate = (1<<16)/(expval/1000000);
    port_def.format.video.nFrameWidth = 1920;
    port_def.format.video.nFrameHeight = 1080;
    port_def.format.video.nStride = 1920;
  } else {
    port_def.format.video.xFramerate = 0;
    port_def.format.video.nFrameWidth = 640;
    port_def.format.video.nFrameHeight = 480;
    port_def.format.video.nStride = 640;
  }
  change_state (camera, OMX_StateIdle);
  wait (camera, EVENT_STATE_SET, 0);
  change_state (camera, OMX_StateLoaded);
  wait (camera, EVENT_STATE_SET, 0);
  if ((error = OMX_SetParameter (camera->handle, OMX_IndexParamPortDefinition,
                                 &port_def))){
    fprintf (stderr, "error: OMX_SetParameter - "
             "OMX_IndexParamPortDefinition: %s", dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  change_state (camera, OMX_StateExecuting);
  wait (camera, EVENT_STATE_SET, 0);
#endif


  fprintf(stderr, "shutterSpeed = %i\n", expval);
  //Exposure value
  OMX_CONFIG_EXPOSUREVALUETYPE exposure_value_st;
  OMX_INIT_STRUCTURE (exposure_value_st);
  exposure_value_st.nPortIndex = OMX_ALL;
  exposure_value_st.eMetering = CAM_METERING;
  exposure_value_st.xEVCompensation = (CAM_EXPOSURE_COMPENSATION << 16)/6;
  exposure_value_st.nShutterSpeedMsec = expval;
  exposure_value_st.bAutoShutterSpeed = CAM_SHUTTER_SPEED_AUTO;
  exposure_value_st.nSensitivity = CAM_ISO;
  exposure_value_st.bAutoSensitivity = CAM_ISO_AUTO;
  if ((error = OMX_SetConfig (camera->handle,
                              OMX_IndexConfigCommonExposureValue, &exposure_value_st))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Bayer data
  if (OMX_TRUE == RAW_BAYER)
  {
    //The filename is not relevant
    char dummy[] = "dummy";
    struct {
      //These two fields need to be together
      OMX_PARAM_CONTENTURITYPE uri_st;
      char padding[5];
    } raw;
    OMX_INIT_STRUCTURE (raw.uri_st);
    raw.uri_st.nSize = sizeof (raw);
    memcpy (raw.uri_st.contentURI, dummy, 5);
    if ((error = OMX_SetConfig (camera->handle,
                                OMX_IndexConfigCaptureRawImageURI, &raw))){
      fprintf (stderr, "error: OMX_SetConfig OMX_IndexConfigCaptureRawImageURI: %s\n", dump_OMX_ERRORTYPE (error));
      exit (1);
    }
  }
}

int main (){
  OMX_ERRORTYPE error;
  OMX_BUFFERHEADERTYPE* encoder_output_buffer;
  component_t camera;
  component_t null_sink;
  component_t encoder;
  camera.name = "OMX.broadcom.camera";
  null_sink.name = "OMX.broadcom.null_sink";
  encoder.name = "OMX.broadcom.image_encode";

#ifdef DBG_PID
  pid_t pid = getpid();
  pid_t tid = syscall(SYS_gettid);
  //pthread_t tid = pthread_self();
  printf("main pid = %i tid = %i\n", pid, tid);
#endif

  openNewFile(0);

  //Initialize Broadcom's VideoCore APIs
  bcm_host_init ();

  //Initialize OpenMAX IL
  if ((error = OMX_Init ())){
    fprintf (stderr, "error: OMX_Init: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Initialize components
  init_component (&camera);
  init_component (&null_sink);
  init_component (&encoder);

  //Initialize camera drivers
  load_camera_drivers (&camera);

  //Configure camera sensor
  printf ("configuring '%s' sensor\n", camera.name);
  OMX_PARAM_SENSORMODETYPE sensor;
  OMX_INIT_STRUCTURE (sensor);
  sensor.nPortIndex = OMX_ALL;
  OMX_INIT_STRUCTURE (sensor.sFrameSize);
  sensor.sFrameSize.nPortIndex = OMX_ALL;
  if ((error = OMX_GetParameter (camera.handle, OMX_IndexParamCommonSensorMode,
      &sensor))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  sensor.bOneShot = OMX_TRUE;
  sensor.sFrameSize.nWidth = CAM_WIDTH;
  sensor.sFrameSize.nHeight = CAM_HEIGHT;
  if ((error = OMX_SetParameter (camera.handle, OMX_IndexParamCommonSensorMode,
      &sensor))){
    fprintf (stderr, "error: OMX_SetParameter6: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  OMX_CONFIG_FRAMERATETYPE framerate;
  OMX_INIT_STRUCTURE(framerate);
  framerate.nPortIndex = 70;
  /* framerate.xEncodeFramerate = (1<<16)/6; */
  if ((error = OMX_GetParameter (camera.handle, OMX_IndexConfigVideoFramerate,
                                 &framerate))){
    fprintf (stderr, "error: OMX_SetParameter6b: %s\n",
             dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  fprintf(stderr, "xEncodeFramerate = %g\n", framerate.xEncodeFramerate/(double)(1<<16));

  /* OMX_CONFIG_FRAMERATETYPE framerate; */
  /* OMX_INIT_STRUCTURE(framerate); */
  /* framerate.nPortIndex = 70; */
  /* framerate.xEncodeFramerate = (1<<16)/6; */
  /* if ((error = OMX_SetParameter (camera.handle, OMX_IndexConfigVideoFramerate, */
  /*     &framerate))){ */
  /*   fprintf (stderr, "error: OMX_SetParameter6b: %s\n", */
  /*       dump_OMX_ERRORTYPE (error)); */
  /*   exit (1); */
  /* } */

  dumpSensorModes(&camera);

  //Configure camera port definition
  printf ("configuring '%s' port definition\n", camera.name);
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  OMX_INIT_STRUCTURE (port_def);
  port_def.nPortIndex = 72;
  if ((error = OMX_GetParameter (camera.handle, OMX_IndexParamPortDefinition,
      &port_def))){
    fprintf (stderr, "error: OMX_GetParameter: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  port_def.format.image.nFrameWidth = CAM_WIDTH;
  port_def.format.image.nFrameHeight = CAM_HEIGHT;
  port_def.format.image.eCompressionFormat = OMX_IMAGE_CodingUnused;
  port_def.format.image.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
  //Stride is byte-per-pixel*width, YUV has 1 byte per pixel, so the stride is
  //the width (rounded up to the nearest multiple of 16).
  //See mmal/util/mmal_util.c, mmal_encoding_width_to_stride()
  port_def.format.image.nStride = round_up (CAM_WIDTH, 32);
  if ((error = OMX_SetParameter (camera.handle, OMX_IndexParamPortDefinition,
      &port_def))){
    fprintf (stderr, "error: OMX_SetParameter7: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Configure preview port
  //In theory the fastest resolution and framerate are 1920x1080 @30fps because
  //these are the default settings for the preview port, so the frames don't
  //need to be resized. In practice, this is not true. The fastest way to
  //produce stills is setting the lowest resolution, that is, 640x480 @30fps.
  //The difference between 1920x1080 @30fps and 640x480 @30fps is a speed boost
  //of ~4%, from ~1083ms to ~1039ms
  port_def.nPortIndex = 70;
  port_def.format.video.eCompressionFormat = OMX_IMAGE_CodingUnused;
  port_def.format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
  //Setting the framerate to 0 unblocks the shutter speed from 66ms to 772ms
  //The higher the speed, the higher the capture time
  //  if (CAM_SHUTTER_SPEED > 1000000)
  {
    port_def.format.video.xFramerate = (1<<16);
    port_def.format.video.nFrameWidth = 1920;
    port_def.format.video.nFrameHeight = 1080;
    port_def.format.video.nStride = 1920;
    /* } else { */
    /*   port_def.format.video.xFramerate = 0; */
    /*   port_def.format.video.nFrameWidth = 640; */
    /*   port_def.format.video.nFrameHeight = 480; */
    /*   port_def.format.video.nStride = 640; */
  }
  if ((error = OMX_SetParameter (camera.handle, OMX_IndexParamPortDefinition,
      &port_def))){
    fprintf (stderr, "error: OMX_SetParameter - "
        "OMX_IndexParamPortDefinition: %s", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Configure camera settings
  set_camera_settings (&camera);

  //Configure encoder port definition
  printf ("configuring '%s' port definition\n", encoder.name);
  OMX_INIT_STRUCTURE (port_def);
  port_def.nPortIndex = 341;
  if ((error = OMX_GetParameter (encoder.handle, OMX_IndexParamPortDefinition,
      &port_def))){
    fprintf (stderr, "error: OMX_SetParameter8: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  port_def.format.image.nFrameWidth = CAM_WIDTH;
  port_def.format.image.nFrameHeight = CAM_HEIGHT;
  port_def.format.image.eCompressionFormat = OMX_IMAGE_CodingJPEG;
  port_def.format.image.eColorFormat = OMX_COLOR_FormatUnused;
  if ((error = OMX_SetParameter (encoder.handle, OMX_IndexParamPortDefinition,
      &port_def))){
    fprintf (stderr, "error: OMX_SetParameter9: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Configure JPEG settings
  set_jpeg_settings (&encoder);

  //Setup tunnels: camera (still) -> image_encode, camera (preview) -> null_sink
  printf ("configuring tunnels\n");
  if ((error = OMX_SetupTunnel (camera.handle, 72, encoder.handle, 340))){
    fprintf (stderr, "error: OMX_SetupTunnel: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }
  if ((error = OMX_SetupTunnel (camera.handle, 70, null_sink.handle, 240))){
    fprintf (stderr, "error: OMX_SetupTunnel: %s\n",
        dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Change state to IDLE
  change_state (&camera, OMX_StateIdle);
  wait (&camera, EVENT_STATE_SET, 0);
  change_state (&null_sink, OMX_StateIdle);
  wait (&null_sink, EVENT_STATE_SET, 0);
  change_state (&encoder, OMX_StateIdle);
  wait (&encoder, EVENT_STATE_SET, 0);

  //  sleep(120);

  {
    OMX_CONFIG_FRAMERATETYPE framerate;
    OMX_INIT_STRUCTURE(framerate);
    framerate.nPortIndex = 70;
    /* framerate.xEncodeFramerate = (1<<16)/6; */
    if ((error = OMX_GetParameter (camera.handle, OMX_IndexConfigVideoFramerate,
                                   &framerate))){
      fprintf (stderr, "error: OMX_SetParameter6b: %s\n",
               dump_OMX_ERRORTYPE (error));
      exit (1);
    }

    fprintf(stderr, "xEncodeFramerate = %g\n", framerate.xEncodeFramerate/(double)(1<<16));
  }
  /* { */
  /* OMX_CONFIG_FRAMERATETYPE framerate; */
  /* OMX_INIT_STRUCTURE(framerate); */
  /* framerate.nPortIndex = 70; */
  /* framerate.xEncodeFramerate = (1<<16)/6; */
  /* if ((error = OMX_SetParameter (camera.handle, OMX_IndexConfigVideoFramerate, */
  /*     &framerate))){ */
  /*   fprintf (stderr, "error: OMX_SetParameter6b: %s\n", */
  /*       dump_OMX_ERRORTYPE (error)); */
  /*   exit (1); */
  /* } */
  /* } */


  //Enable the tunnel ports
  enable_port (&camera, 70);
  enable_port (&null_sink, 240);
  wait (&null_sink, EVENT_PORT_ENABLE, 0);

  enable_port (&camera, 72);
  wait (&camera, EVENT_PORT_ENABLE, 0);
  enable_port (&encoder, 340);
  wait (&encoder, EVENT_PORT_ENABLE, 0);
  enable_encoder_output_port (&encoder, &encoder_output_buffer);

  /* { */
  /* OMX_CONFIG_FRAMERATETYPE framerate; */
  /* OMX_INIT_STRUCTURE(framerate); */
  /* framerate.nPortIndex = 70; */
  /* framerate.xEncodeFramerate = (1<<16)/6; */
  /* if ((error = OMX_SetParameter (camera.handle, OMX_IndexConfigVideoFramerate, */
  /*     &framerate))){ */
  /*   fprintf (stderr, "error: OMX_SetParameter6b: %s\n", */
  /*       dump_OMX_ERRORTYPE (error)); */
  /*   exit (1); */
  /* } */
  /* } */

  //Change state to EXECUTING
  change_state (&camera, OMX_StateExecuting);
  wait (&camera, EVENT_STATE_SET, 0);
  change_state (&null_sink, OMX_StateExecuting);
  wait (&null_sink, EVENT_STATE_SET, 0);
  change_state (&encoder, OMX_StateExecuting);
  wait (&encoder, EVENT_STATE_SET, 0);

  OMX_CONFIG_PORTBOOLEANTYPE cameraCapturePort;
  OMX_INIT_STRUCTURE (cameraCapturePort);
  sleep(2);
  //Start consuming the buffers
  VCOS_UNSIGNED end_flags = EVENT_BUFFER_FLAG | EVENT_FILL_BUFFER_DONE;
  VCOS_UNSIGNED retrieves_events;
  //Enable camera capture port. This basically says that the port 72 will be
  //used to get data from the camera. If you're capturing video, the port 71
  //must be used
  printf ("enabling '%s' capture port\n", camera.name);
  cameraCapturePort.nPortIndex = 72;
  cameraCapturePort.bEnabled = OMX_TRUE;
  if ((error = OMX_SetConfig (camera.handle, OMX_IndexConfigPortCapturing,
      &cameraCapturePort))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  int i = 0;
  while (1){
    while (1){
      //Get the buffer data (a slice of the image)
      if ((error = OMX_FillThisBuffer (encoder.handle, encoder_output_buffer))){
        fprintf (stderr, "error: OMX_FillThisBuffer: %s\n",
                 dump_OMX_ERRORTYPE (error));
        exit (1);
      }

      //Wait until it's filled
      wait (&encoder, EVENT_FILL_BUFFER_DONE, &retrieves_events);

      //Append the buffer into the file
      if (pwrite (fd, encoder_output_buffer->pBuffer,
                  encoder_output_buffer->nFilledLen,
                  encoder_output_buffer->nOffset) == -1){
        fprintf (stderr, "error: pwrite\n");
        exit (1);
      }

      fprintf(stderr, "LOOP event = %i\n", retrieves_events);

      //When it's the end of the stream, an OMX_EventBufferFlag is emitted in the
      //camera and image_encode components. Then the FillBufferDone function is
      //called in the image_encode
      if (retrieves_events == end_flags){
        //Clear the EOS flags
        wait (&camera, EVENT_BUFFER_FLAG, 0);
        wait (&encoder, EVENT_BUFFER_FLAG, 0);

        break;
      }
    }
    closeFile();
    if (18<++i) break;
    int speed = 1000000>>(18-i);
    printf ("------NEXT FRAME------------------------------------------\n");
    openNewFile(speed);

    setExp(&camera, speed);

    printf ("enabling '%s' capture port\n", camera.name);
    cameraCapturePort.nPortIndex = 72;
    cameraCapturePort.bEnabled = OMX_TRUE;
    if ((error = OMX_SetConfig (camera.handle, OMX_IndexConfigPortCapturing,
                                &cameraCapturePort))){
      fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
      exit (1);
    }

  }
  printf ("------------------------------------------------\n");

  //Disable camera capture port
  printf ("disabling '%s' capture port\n", camera.name);
  cameraCapturePort.bEnabled = OMX_FALSE;
  if ((error = OMX_SetConfig (camera.handle, OMX_IndexConfigPortCapturing,
                              &cameraCapturePort))){
    fprintf (stderr, "error: OMX_SetConfig: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Change state to IDLE
  change_state (&camera, OMX_StateIdle);
  wait (&camera, EVENT_STATE_SET, 0);
  change_state (&null_sink, OMX_StateIdle);
  wait (&null_sink, EVENT_STATE_SET, 0);
  change_state (&encoder, OMX_StateIdle);
  wait (&encoder, EVENT_STATE_SET, 0);

  //Disable the tunnel ports
  disable_port (&camera, 72);
  disable_port (&camera, 70);
  disable_port (&null_sink, 240);
  disable_port (&encoder, 340);
  disable_encoder_output_port (&encoder, encoder_output_buffer);

  //Change state to LOADED
  change_state (&camera, OMX_StateLoaded);
  wait (&camera, EVENT_STATE_SET, 0);
  change_state (&null_sink, OMX_StateLoaded);
  wait (&null_sink, EVENT_STATE_SET, 0);
  change_state (&encoder, OMX_StateLoaded);
  wait (&encoder, EVENT_STATE_SET, 0);

  //Deinitialize components
  deinit_component (&camera);
  deinit_component (&null_sink);
  deinit_component (&encoder);

  //Deinitialize OpenMAX IL
  if ((error = OMX_Deinit ())){
    fprintf (stderr, "error: OMX_Deinit: %s\n", dump_OMX_ERRORTYPE (error));
    exit (1);
  }

  //Deinitialize Broadcom's VideoCore APIs
  bcm_host_deinit ();

  printf ("ok\n");

  return 0;
}
