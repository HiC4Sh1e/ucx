/*
 * Copyright (C) Mellanox Technologies Ltd. 2019. ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */
package org.openucx.jucx.ucp;

import org.openucx.jucx.UcxException;
import org.openucx.jucx.UcxNativeStruct;

import java.io.Closeable;

/**
 * The listener handle is an opaque object that is used for listening on a
 * specific address and accepting connections from clients.
 */
public class UcpListener extends UcxNativeStruct implements Closeable {

    public UcpListener(UcpWorker worker, UcpListenerParams params) {
        if (params.getSockAddr() == null) {
            throw new UcxException("UcpListenerParams.sockAddr must be non-null.");
        }
        setNativeId(createUcpListener(params, worker.getNativeId()));
    }

    @Override
    public void close() {
        destroyUcpListenerNative(getNativeId());
        setNativeId(null);
    }

    private static native long createUcpListener(UcpListenerParams params, long workerId);

    private static native void destroyUcpListenerNative(long listenerId);
}