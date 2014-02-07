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
      return list.last().compare(QString("WAV"),Qt::CaseInsensitive);
    }
    else return false;
}

int Sound::getSampleRate(QByteArray& rate) const {
  int sampleRate = 0;
  for(int i=0; i<rate.size(); i++){
    sampleRate += rate[i]>>4 * (16*i) + (rate[i] & 15) * i;
  }
  return sampleRate;
}

void Sound::resample(QByteArray array) {

}

bool Sound::convertWAVtoAudioMixerInput(QByteArray& array){
  QByteArray wav = QByteArray(array.at(8), 4);
  //  if(wav!="WAV") return false;
  
  QByteArray sampleRateBytes = QByteArray(array.at(24), 4);
  int sampleRate = getSampleRate(sampleRateBytes);
  if(sampleRate == 48000){
    array.resize(array.size() / 2);
  } else {
    resample(sampleRateBytes);
  }
  return true;
}

void Sound::replyFinished(QNetworkReply* reply) {
    // replace our byte array with the downloaded data
    QByteArray rawAudioByteArray = reply->readAll();


    if(WAVExtension){
      convertWAVtoAudioMixerInput(rawAudioByteArray);
    }
    else {

      // assume that this was a RAW file and is now an array of samples that are
      // signed, 16-bit, 48Khz, mono

      // we want to convert it to the format that the audio-mixer wants
      // which is signed, 16-bit, 24Khz, mono

      _byteArray.resize(rawAudioByteArray.size() / 2);

      int numSourceSamples = rawAudioByteArray.size() / sizeof(int16_t);
      int16_t* sourceSamples = (int16_t*) rawAudioByteArray.data();
      int16_t* destinationSamples = (int16_t*) _byteArray.data();

      for (int i = 1; i < numSourceSamples; i += 2) {
        if (i + 1 >= numSourceSamples) {
	  destinationSamples[(i - 1) / 2] = (sourceSamples[i - 1] / 2) + (sourceSamples[i] / 2);
        } else {
	  destinationSamples[(i - 1) / 2] = (sourceSamples[i - 1] / 4) + (sourceSamples[i] / 2) + (sourceSamples[i + 1] / 4);
        }
      }
    }
}
