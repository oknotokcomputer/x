// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package proxy

import (
	"fmt"
	"strings"

	"chromiumos/dbusbindings/dbustype"
	"chromiumos/dbusbindings/generate/genutil"
	"chromiumos/dbusbindings/introspect"
)

type param struct {
	Type, Name string
}

func makeMethodParams(offset int, args []introspect.MethodArg) ([]param, error) {
	var ret []param
	for i, a := range args {
		argType, prefix := a.InArgType, "in"
		if a.Direction == "out" {
			argType, prefix = a.OutArgType, "out"
		}
		t, err := argType(dbustype.ReceiverProxy)
		if err != nil {
			return nil, err
		}
		ret = append(ret, param{t, genutil.ArgName(prefix, a.Name, i+offset)})
	}

	return ret, nil
}

func makeMethodCallbackType(args []introspect.MethodArg) (string, error) {
	var params []string
	for _, a := range args {
		t, err := a.CallbackType()
		if err != nil {
			return "", err
		}
		params = append(params, fmt.Sprintf("%s /*%s*/", t, a.Name))
	}
	return fmt.Sprintf("base::OnceCallback<void(%s)>", strings.Join(params, ", ")), nil

}

// Returns stringified C++ type for signal callback.
func makeSignalCallbackType(args []introspect.SignalArg) (string, error) {
	if len(args) == 0 {
		return "base::RepeatingClosure", nil
	}

	var lines []string
	for _, a := range args {
		line, err := a.CallbackType()
		if err != nil {
			return "", err
		}
		lines = append(lines, line)
	}
	const (
		prefix = "const base::RepeatingCallback<void("
		suffix = ")>&"
	)
	indent := strings.Repeat(" ", len(prefix))
	return fmt.Sprintf("%s%s%s", prefix, strings.Join(lines, ",\n"+indent), suffix), nil
}

// extractInterfacesWithProperties returns an array of Interfaces that have Properties.
func extractInterfacesWithProperties(iss []introspect.Introspection) []introspect.Interface {
	var ret []introspect.Interface
	for _, is := range iss {
		for _, itf := range is.Interfaces {
			if len(itf.Properties) > 0 {
				ret = append(ret, itf)
			}
		}
	}
	return ret
}
