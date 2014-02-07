//
//  Sound.cpp
//  hifi
//
//  Created by Stephen Birarda on 1/2/2014.
//  Copyright (c) 2014 HighFidelity, Inc. All rights reserved.
//

#include <stdint.h>

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>

#include "Sound.h"

Sound::Sound(const QUrl& sampleURL, QObject* parent) :
    QObject(parent)
{
    // Assume WAV and RAW input formats only and in correct file extension
    WAVExtension = isFileExtensionWAV(sampleURL); 

    // assume we have a QApplication or QCoreApplication instance and use the
    // QNetworkAccess manager to grab the raw audio file at the given URL

    QNetworkAccessManager *manager = new QNetworkAccessManager(this);
    connect(manager, SIGNAL(finished(QNetworkReply*)),
            this, SLOT(replyFinished(QNetworkReply*)));

    manager->get(QNetworkRequest(sampleURL));
}

bool Sound::isFileExtensionWAV(const QUrl& sampleURL) const{
  
    const QStringList list = sampleURL.toString().split(".");
    if(!list.isEmpty()){
      return !list.last().compare(QString("WAV"),Qt::CaseInsensitive);
    }
    else return false;
}

int Sound::getSampleRate(QByteArray& rate) const {
  int sampleRate = 0;
  for(int i=0; i<rate.size(); i++){
    sampleRate += ((rate[i]>>4<<4) | (rate[i] & 15)) << i*8;  
  }
  return sampleRate;
}

void Sound::resample(QByteArray& array, int sampleRate) {
  // assume that this is a RAW file and is now an array of samples that are
  // signed, 16-bit, 48Khz, mono

  // we want to convert it to the format that the audio-mixer wants
  // which is signed, 16-bit, 24Khz, mono

  if(sampleRate == 48000){
    _byteArray.resize(array.size() / 2);

    int numSourceSamples = array.size() / sizeof(int16_t);
    int16_t* sourceSamples = (int16_t*) array.data();
    int16_t* destinationSamples = (int16_t*) _byteArray.data();

    for (int i = 1; i < numSourceSamples; i += 2) {
      if (i + 1 >= numSourceSamples) {
	destinationSamples[(i - 1) / 2] = (sourceSamples[i - 1] / 2) + (sourceSamples[i] / 2);
      } else {
	destinationSamples[(i - 1) / 2] = (sourceSamples[i - 1] / 4) + (sourceSamples[i] / 2) + (sourceSamples[i + 1] / 4);
      }
    }
  } 
  else {
    //not implemented yet;
    _byteArray = array;
  }

}

int Sound::convertWAVtoRAW(QByteArray& array){
  array.remove(0,24); // remove bytes up to sample rate at byte 24
  QByteArray sampleRateBytes = QByteArray(array[0], 4);
  int sampleRate = getSampleRate(sampleRateBytes);
  array.remove(0,20); // remove bytes to RAW data at byte 44
  return sampleRate;
}

void Sound::replyFinished(QNetworkReply* reply) {
    // replace our byte array with the downloaded data
    QByteArray rawAudioByteArray = reply->readAll();
    int sampleRate = 48000;
    if(WAVExtension){
      sampleRate = convertWAVtoRAW(rawAudioByteArray);
    }

    resample(rawAudioByteArray, sampleRate);

}
