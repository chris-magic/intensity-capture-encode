#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

#include "DeckLinkAPI.h"
#include "Capture.h"
#include "Capture_global.h"

extern "C"{
//#include "chris_global.h"
//#include "chris_error.h"
#ifndef   UINT64_C

#define   UINT64_C(value)__CONCAT(value,ULL)

#endif
#include "output_handle.h"
#include "libswscale/swscale.h"
}

//may be ,all the following variable can be remove

DeckLinkCaptureDelegate::DeckLinkCaptureDelegate() : m_refCount(0)
{
	pthread_mutex_init(&m_mutex, NULL);
}

DeckLinkCaptureDelegate::~DeckLinkCaptureDelegate()
{
	pthread_mutex_destroy(&m_mutex);
}

ULONG DeckLinkCaptureDelegate::AddRef(void)
{
	pthread_mutex_lock(&m_mutex);
		m_refCount++;
	pthread_mutex_unlock(&m_mutex);

	return (ULONG)m_refCount;
}

ULONG DeckLinkCaptureDelegate::Release(void)
{
	pthread_mutex_lock(&m_mutex);
		m_refCount--;
	pthread_mutex_unlock(&m_mutex);

	if (m_refCount == 0)
	{
		delete this;
		return 0;
	}

	return (ULONG)m_refCount;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioFrame)
{
	void*					frameBytes;
	void*					audioFrameBytes;
	
	/* Video Frame	*/
	if(videoFrame)
	{	

		if (videoFrame->GetFlags() & bmdFrameHasNoInputSource)
		{
			fprintf(stderr, "Frame received (#%u) - No input signal detected\n", this->seg_union->picture_capture_no);
		}
		else
		{
			videoFrame->GetBytes(&frameBytes);

			if( pthread_mutex_trylock(&this->yuv_video_buf->yuv_buf_mutex) == 0){ //lock sucess

				printf("have_data_mark = %d \n" ,this->yuv_video_buf->have_data_mark);
				if(this->yuv_video_buf->have_data_mark == 0){
					this->yuv_video_buf->yuv_data = (unsigned char *)frameBytes;

					this->yuv_video_buf->have_data_mark = 1; // not set
					pthread_cond_signal(&this->yuv_video_buf->yuv_buf_cond);
				}
				else{//others ,drop
					printf("...........................\n");
				}
				pthread_mutex_unlock(&this->yuv_video_buf->yuv_buf_mutex);
			}
		}

		this->seg_union->picture_capture_no ++ ;
	}

	/* Handle Audio Frame */
	if (audioFrame)
	{
		//printf("audio .... ,frame count = %ld\n" ,audioFrame->GetSampleFrameCount());
		int haha = audioFrame->GetSampleFrameCount() * CAPTURE_AUDIO_CHANNEL_NUM * (CAPTURE_AUDIO_SAMPLE_DEPTH / 8);
		audioFrame->GetBytes(&audioFrameBytes);
		do_audio_out(this->seg_union->output_ctx ,audioFrameBytes
						,audioFrame->GetSampleFrameCount() * CAPTURE_AUDIO_CHANNEL_NUM * (CAPTURE_AUDIO_SAMPLE_DEPTH / 8)
						,audioFrame->GetSampleFrameCount());
//			write(audioOutputFile, audioFrameBytes, audioFrame->GetSampleFrameCount() * AUDIO_CHANNEL_NUM * (AUDIO_SAMPLE_DEPTH / 8));
	}
    return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode, BMDDetectedVideoInputFormatFlags)
{
    return S_OK;
}

void * encode_yuv_data( void *void_del){

	DeckLinkCaptureDelegate 	*delegate  = (DeckLinkCaptureDelegate 	*)void_del;
	while(1){

		//1.get lock
		pthread_mutex_lock(&delegate->yuv_video_buf->yuv_buf_mutex);

		if(delegate->yuv_video_buf->have_data_mark == 0){
			printf("encode wait ...\n");
			pthread_cond_wait(&delegate->yuv_video_buf->yuv_buf_cond ,&delegate->yuv_video_buf->yuv_buf_mutex);
			printf("after wait ...\n");
		}
		printf("after wait 1.. ,have_data_mark = %d.\n" ,delegate->yuv_video_buf->have_data_mark);
//		//encode
		seg_write_frame(delegate->seg_union ,
						delegate->seg_union->width_capture ,delegate->seg_union->height_caputre ,
						VIDEO_STREAM_FLAG  ,delegate->yuv_video_buf->yuv_data );

		//change mark
		delegate->yuv_video_buf->have_data_mark = 0;

		//unlock lock
		pthread_mutex_unlock(&delegate->yuv_video_buf->yuv_buf_mutex);

	}

	return NULL;
}

/*
 * callback thread in charge of to capture the video and audio data ,and then put they into pipe;
 *
 * create new thread to read the data from the pipe ,and encode ...
 *
 * in sum : 3 threads
 * */
int main(int argc, char *argv[])
{

	IDeckLink 						*deckLink;
	IDeckLinkInput					*deckLinkInput;
	IDeckLinkDisplayModeIterator	*displayModeIterator;
	//
	IDeckLinkIterator			*deckLinkIterator = CreateDeckLinkIteratorInstance();
	DeckLinkCaptureDelegate 	*delegate;
	IDeckLinkDisplayMode		*displayMode;
	BMDVideoInputFlags			inputFlags = 0;
	BMDDisplayMode				selectedDisplayMode = bmdModeNTSC;
	BMDPixelFormat				pixelFormat = bmdFormat8BitYUV;    //mode 11 only support this pixel format
	int							displayModeCount = 0;
	int							ch;
	bool 						foundDisplayMode = false;
	HRESULT						result;
	

//==============================================
	//define a struct point variable
	Segment_U * seg_union = NULL;
	/*Segment union */
	init_seg_union(&seg_union ,argc ,argv);

	seg_write_header(seg_union);

//===========================

	if (!deckLinkIterator)
	{
		fprintf(stderr, "This application requires the DeckLink drivers installed.\n");
		goto bail;
	}
	
	/* Connect to the first DeckLink instance */
	result = deckLinkIterator->Next(&deckLink);   //traverse decklink card ...
	//The  IDeckLink  object interface represents a physical DeckLink device attached to the host computer.
	if (result != S_OK)
	{
		fprintf(stderr, "No DeckLink PCI cards found.\n");
		goto bail;
	}
    
	if (deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&deckLinkInput) != S_OK)
		goto bail;


	delegate = new DeckLinkCaptureDelegate();
	delegate->seg_union = seg_union;		//set seg_union
	deckLinkInput->SetCallback(delegate);	//Register input callback
   
	result = deckLinkInput->GetDisplayModeIterator(&displayModeIterator);
	if (result != S_OK)
	{
		fprintf(stderr, "Could not obtain the video output display mode iterator - result = %08x\n", result);
		goto bail;
	}
	
	//in here ,after init work
	printf("\n\n in here ,after init work  ,g_videoModeIndex = %d \n\n" ,VIDEO_MODE_INDEX);

	/*
	 * The  IDeckLinkDisplayModeIterator  object interface is used to enumerate the available
		display modes for a DeckLink device.
	 * */
	while (displayModeIterator->Next(&displayMode) == S_OK)//The Next method returns the next available IDeckLinkDisplayMode interface.
	{

		printf("g_videoModeIndex = %d ,displayModeCount = %d \n" ,VIDEO_MODE_INDEX ,displayModeCount);

		if (VIDEO_MODE_INDEX == displayModeCount)
		{
			BMDDisplayModeSupport result;
			const char *displayModeName;
			
			foundDisplayMode = true;
			displayMode->GetName(&displayModeName);
			printf(" displayModeName = %s \n" ,displayModeName);

			selectedDisplayMode = displayMode->GetDisplayMode();
			
			printf("selectedDisplayMode = %x \n\n" , selectedDisplayMode);
			deckLinkInput->DoesSupportVideoMode(selectedDisplayMode, pixelFormat, bmdVideoInputFlagDefault, &result, NULL);


			delegate->seg_union->width_capture = (int)displayMode->GetWidth();
			delegate->seg_union->height_caputre = (int)displayMode->GetHeight();

			printf("result = %u \n\n" ,result);
			if (result == bmdDisplayModeNotSupported)
			{
				fprintf(stderr, "The display mode %s is not supported with the selected pixel format\n", displayModeName);
				exit(NOT_SUPPORT_MODE);
			}

			break;
		}
		displayModeCount++;
		displayMode->Release();
	} //end while

	if (!foundDisplayMode)
	{
		fprintf(stderr, "Invalid mode %d specified\n", VIDEO_MODE_INDEX);
		exit(INVALID_MODE);
	}

	//*****************************************************************
	//print width ,height capture from the device
	printf( "width = %d ,height = %d \n" ,delegate->seg_union->width_capture ,delegate->seg_union->height_caputre);
	/*	init yuv_video_buffer*/
	delegate->yuv_video_buf = (yuv_video_buf_union * )malloc(sizeof(yuv_video_buf_union));
	if(delegate->yuv_video_buf == NULL){
		printf("yuv video buf malloc failed .\n");
		exit(1);
	}

	delegate->yuv_video_buf->yuv_data = (unsigned char *)malloc(delegate->seg_union->width_capture * delegate->seg_union->height_caputre * 2);
	if(delegate->yuv_video_buf->yuv_data == NULL){
		printf("yuv_data buffer malloc failed .\n");
		exit(1);
	}

	//take img_conver_ctx from here
	delegate->seg_union->output_ctx->img_convert_ctx = sws_getContext(
			delegate->seg_union->width_capture ,delegate->seg_union->height_caputre ,PIX_FMT_UYVY422,
			delegate->seg_union->output_ctx->video_stream->codec->width ,delegate->seg_union->output_ctx->video_stream->codec->height ,PIX_FMT_YUV420P ,
			 SWS_BICUBIC ,NULL ,NULL ,NULL);

	pthread_mutex_init(&delegate->yuv_video_buf->yuv_buf_mutex, NULL);
	pthread_cond_init(&delegate->yuv_video_buf->yuv_buf_cond, NULL);
	delegate->yuv_video_buf->have_data_mark = 0;

	//new a thread to encode video data
	pthread_t pid_video_encode;
	pthread_create(&pid_video_encode , NULL ,encode_yuv_data ,delegate);
	//*****************************************************************
	/*
	 *The  EnableVideoInput method configures video input and
	 *puts the hardware into video capture mode.
	 *Video input (and optionally audio input) is started by calling  StartStreams .
	 * */

	printf("inputFlags = %u \n" ,inputFlags);
    result = deckLinkInput->EnableVideoInput(selectedDisplayMode, pixelFormat, inputFlags);
    if(result != S_OK)
    {
		fprintf(stderr, "Failed to enable video input. Is another application using the card?\n");
        goto bail;
    }

    /*
     * The  EnableAudioInput method configures audio input
     * and puts the hardware into audio capture mode.
     *  Synchronized audio and video input is started by calling StartStreams .
     * */
    result = deckLinkInput->EnableAudioInput(bmdAudioSampleRate48kHz, CAPTURE_AUDIO_SAMPLE_DEPTH, CAPTURE_AUDIO_CHANNEL_NUM);
    if(result != S_OK)
    {
    	printf("audion enable failed ...\n");
        goto bail;
    }

    /*
     * The  StartStreams  method starts synchronized video and
     * audio capture as configured with EnableVideoInput and optionally  EnableAudioInput.
     * */
	result = deckLinkInput->StartStreams();
    if(result != S_OK)
    {
        goto bail;
    }

    pthread_join(pid_video_encode ,NULL);
	// Block main thread until signal occurs

bail:
   	
	if (displayModeIterator != NULL)
	{
		displayModeIterator->Release();
		displayModeIterator = NULL;
	}

    if (deckLinkInput != NULL)
    {
        deckLinkInput->Release();
        deckLinkInput = NULL;
    }

    if (deckLink != NULL)
    {
        deckLink->Release();
        deckLink = NULL;
    }

	if (deckLinkIterator != NULL)
		deckLinkIterator->Release();

    return 0;
}

