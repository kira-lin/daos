//
// (C) Copyright 2018 Intel Corporation.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
// The Government's rights to use, modify, reproduce, release, perform, display,
// or disclose this software are subject to the terms of the Apache License as
// provided in Contract No. 8F-30005.
// Any reproduction of computer software, computer software documentation, or
// portions thereof marked with this legend must also reproduce the markings.
//

package mgmt

import (
	"fmt"

	"github.com/golang/protobuf/proto"
	"golang.org/x/net/context"

	pb "github.com/daos-stack/daos/src/control/mgmt/proto"
)

// FeatureMap is a type alias
type FeatureMap map[string]*pb.Feature

// GetFeature returns the feature from feature name.
func (s *ControlService) GetFeature(
	ctx context.Context, name *pb.FeatureName) (*pb.Feature, error) {
	f, exists := s.SupportedFeatures[name.Name]
	if !exists {
		return nil, fmt.Errorf("no feature with name %s", name.Name)
	}
	return f, nil
}

// ListAllFeatures lists all features supported by the management server.
func (s *ControlService) ListAllFeatures(
	empty *pb.EmptyParams, stream pb.MgmtControl_ListAllFeaturesServer) error {
	for _, feature := range s.SupportedFeatures {
		if err := stream.Send(feature); err != nil {
			return err
		}
	}
	return nil
}

// ListFeatures lists all features supported by the management server.
func (s *ControlService) ListFeatures(
	category *pb.Category, stream pb.MgmtControl_ListFeaturesServer) error {
	for _, feature := range s.SupportedFeatures {
		if proto.Equal(feature.GetCategory(), category) {
			if err := stream.Send(feature); err != nil {
				return err
			}
		}
	}
	return nil
}
