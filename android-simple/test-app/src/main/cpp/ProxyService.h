#pragma once

#include <jni.h>

extern "C" {

JNIEXPORT jint JNICALL
Java_org_apache_trafficserver_test_TrafficServerProxyService_startProxy(
        JNIEnv *env, jobject thiz, jstring configPath);

JNIEXPORT void JNICALL
Java_org_apache_trafficserver_test_TrafficServerProxyService_stopProxy(
        JNIEnv *env, jobject thiz);

JNIEXPORT jboolean JNICALL
Java_org_apache_trafficserver_test_TrafficServerProxyService_isProxyRunning(
        JNIEnv *env, jobject thiz);

}
