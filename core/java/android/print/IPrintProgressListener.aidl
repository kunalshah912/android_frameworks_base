/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.print;

import android.os.ICancellationSignal;
import android.print.PageRange;
import android.print.PrintAdapterInfo;

/**
 * Callbacks for observing the print progress (writing of printed content)
 * of a PrintAdapter.
 *
 * @hide
 */
oneway interface IPrintProgressListener {
    void onWriteStarted(in PrintAdapterInfo info, ICancellationSignal cancellationSignal);
    void onWriteFinished(in List<PageRange> pages);
    void onWriteFailed(CharSequence error);
}
