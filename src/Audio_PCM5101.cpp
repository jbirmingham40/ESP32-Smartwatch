#include "Audio_PCM5101.h"

#define AUDIO_TICK_PERIOD_MS 1

Audio audio;
static esp_timer_handle_t s_audio_tick_timer = NULL;

uint8_t Volume = Volume_MAX;

void increase_audio_tick(void *arg)
{
  if (audio.isRunning())
    audio.loop();
  else
    esp_timer_stop(s_audio_tick_timer);
}

void Audio_Init() {
  // Audio
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(Volume); // 0...21    

  const esp_timer_create_args_t audio_tick_timer_args = {
    .callback = &increase_audio_tick,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "audio_tick",
    .skip_unhandled_events = true
  };
  esp_timer_create(&audio_tick_timer_args, &s_audio_tick_timer);
}

void Audio_StopTickTimer(void) {
  if (s_audio_tick_timer) esp_timer_stop(s_audio_tick_timer);
}

void Audio_StartTickTimer(void) {
  if (s_audio_tick_timer) esp_timer_start_periodic(s_audio_tick_timer, AUDIO_TICK_PERIOD_MS * 1000);
}

void Volume_adjustment(uint8_t Volume) {
  if(Volume > Volume_MAX )
    printf("Audio : The volume value is incorrect. Please enter 0 to 21\r\n");
  else
    audio.setVolume(Volume); // 0...21    
}

void Play_Music_test() {
  // SD Card
  if (SD_MMC.exists("/A.mp3")) {
    printf("File 'A.mp3' found in root directory.\r\n");
  } else {
    printf("File 'A.mp3' not found in root directory.\r\n");
  }
  bool ret = audio.connecttoFS(SD_MMC,"/A.mp3");
  if(ret) {
    printf("Music Read OK\r\n");
    Audio_StartTickTimer();
  } else
    printf("Music Read Failed\r\n");
}

void Play_Music(const char* directory, const char* fileName) {
  // SD Card
  if (!File_Search(directory,fileName) ) {
    printf("%s file not found.\r\n",fileName);
  }
  const int maxPathLength = 100; 
  char filePath[maxPathLength];
  if (strcmp(directory, "/") == 0) {                                               
    snprintf(filePath, maxPathLength, "%s%s", directory, fileName);   
  } else {                                                            
    snprintf(filePath, maxPathLength, "%s/%s", directory, fileName);
  }
  // printf("%s AAAAAAAA.\r\n",filePath);    
  if(!audio.isRunning()) {
    bool ret = audio.connecttoFS(SD_MMC,(char*)filePath);
    if(ret) {
      printf("Music Read OK\r\n");
      Audio_StartTickTimer();
    } else
      printf("Music Read Failed\r\n");
  }
  vTaskDelay(pdMS_TO_TICKS(100));    
}
void Music_pause() {
  if (audio.isRunning()) {            
    audio.pauseResume();             
    printf("The music pause\r\n");
  }
}
void Music_resume() {
  if (!audio.isRunning()) {
    audio.pauseResume();
    Audio_StartTickTimer();
    printf("The music begins\r\n");
  }
}

uint32_t Music_Duration() {
  uint32_t Audio_duration = audio.getAudioFileDuration(); 
  // Audio_duration = 360;
  if(Audio_duration > 60)
    printf("Audio duration is %d minutes and %d seconds\r\n",Audio_duration/60,Audio_duration%60);
  else{
    if(Audio_duration != 0)
      printf("Audio duration is %d seconds\r\n",Audio_duration);
    else
      printf("Fail : Failed to obtain the audio duration.\r\n");
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  return Audio_duration;
}
uint32_t Music_Elapsed() {
  uint32_t Audio_elapsed = audio.getAudioCurrentTime(); 
  return Audio_elapsed;
}

void Audio_Loop()
{
  if(!audio.isRunning())
    Play_Music_test();
}


