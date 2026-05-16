import SwiftUI

struct MatchScoreSliderSection: View {
    @ObservedObject var viewModel: SettingsViewModel
    
    // Values representing DISTANCE (1.0 - similarity)
    @State private var duplicateDistance: Double = 0.02
    @State private var similarDistance: Double = 0.10
    
    // Max distance on the slider scale
    let maxDistance: Double = 0.20
    
    var body: some View {
        VStack(alignment: .leading, spacing: 15) {
            HStack {
                Text("Match Score Thresholds")
                    .font(.title2)
                    .fontWeight(.semibold)
                    .foregroundColor(.primary)
                
                Button(action: {
                    // Placeholder for popover if needed
                }) {
                    Image(systemName: "info.circle")
                        .foregroundColor(.secondary)
                }
                .buttonStyle(PlainButtonStyle())
                .help("Adjust the strictness for grouping images and identifying exact duplicates. A lower score requires a closer match.")
            }
            
            DualSlider(
                duplicateValue: $duplicateDistance,
                similarValue: $similarDistance,
                range: 0.0...maxDistance
            )
            .frame(height: 120)
            .padding(.vertical, 10)
            
            HStack {
                Text("Exact Duplicate Distance: \(String(format: "%.4f", duplicateDistance))")
                    .font(.caption)
                    .foregroundColor(.secondary)
                Spacer()
                Text("Similar Image Distance: \(String(format: "%.4f", similarDistance))")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
        }
        .padding()
        .background(Color(NSColor.controlBackgroundColor))
        .cornerRadius(10)
        .onAppear {
            loadValues()
        }
        .onChange(of: duplicateDistance) { _ in saveValues() }
        .onChange(of: similarDistance) { _ in saveValues() }
        .onChange(of: viewModel.config["duplicates.threshold.duplicate"]) { _ in loadValues() }
        .onChange(of: viewModel.config["duplicates.threshold.similarity"]) { _ in loadValues() }
    }
    
    private func loadValues() {
        if let dupStr = viewModel.config["duplicates.threshold.duplicate"], let dupVal = Double(dupStr) {
            let dist = 1.0 - dupVal
            if abs(duplicateDistance - dist) > 0.0001 {
                duplicateDistance = min(max(0.0, dist), maxDistance)
            }
        }
        if let simStr = viewModel.config["duplicates.threshold.similarity"], let simVal = Double(simStr) {
            let dist = 1.0 - simVal
            if abs(similarDistance - dist) > 0.0001 {
                similarDistance = min(max(0.0, dist), maxDistance)
            }
        }
    }
    
    private func saveValues() {
        let dupVal = 1.0 - duplicateDistance
        let simVal = 1.0 - similarDistance
        
        let newDupStr = String(format: "%.5f", dupVal)
        let newSimStr = String(format: "%.5f", simVal)
        
        if viewModel.config["duplicates.threshold.duplicate"] != newDupStr {
            viewModel.config["duplicates.threshold.duplicate"] = newDupStr
            viewModel.modifiedProperties.insert("duplicates.threshold.duplicate")
        }
        
        if viewModel.config["duplicates.threshold.similarity"] != newSimStr {
            viewModel.config["duplicates.threshold.similarity"] = newSimStr
            viewModel.modifiedProperties.insert("duplicates.threshold.similarity")
        }
    }
}

struct DualSlider: View {
    @Binding var duplicateValue: Double
    @Binding var similarValue: Double
    let range: ClosedRange<Double>
    
    var body: some View {
        GeometryReader { geometry in
            let width = geometry.size.width
            let height = geometry.size.height
            let centerY = height / 2
            let thumbSize: CGFloat = 16
            
            ZStack(alignment: .topLeading) {
                // Background Track
                RoundedRectangle(cornerRadius: 4)
                    .fill(Color(NSColor.separatorColor))
                    .frame(width: width, height: 6)
                    .offset(y: centerY - 3)
                
                // Similar Images Range Track (Light Blue)
                RoundedRectangle(cornerRadius: 4)
                    .fill(Color.blue.opacity(0.3))
                    .frame(width: max(0, xPosition(for: similarValue, in: width)), height: 6)
                    .offset(y: centerY - 3)
                
                // Duplicates Range Track (Solid Blue)
                RoundedRectangle(cornerRadius: 4)
                    .fill(Color.blue)
                    .frame(width: max(0, xPosition(for: duplicateValue, in: width)), height: 6)
                    .offset(y: centerY - 3)
                
                // Top Bracket for Similar Images
                Path { path in
                    let endX = max(0, xPosition(for: similarValue, in: width))
                    let startY = centerY - 22
                    let midY = centerY - 30
                    let topY = centerY - 38
                    
                    if endX > 10 {
                        path.move(to: CGPoint(x: 0, y: startY))
                        path.addLine(to: CGPoint(x: 0, y: midY))
                        path.addLine(to: CGPoint(x: endX/2 - 5, y: midY))
                        path.addLine(to: CGPoint(x: endX/2, y: topY))
                        path.addLine(to: CGPoint(x: endX/2 + 5, y: midY))
                        path.addLine(to: CGPoint(x: endX, y: midY))
                        path.addLine(to: CGPoint(x: endX, y: startY))
                    }
                }
                .stroke(Color.secondary, lineWidth: 1)
                
                Text("Similar Images")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .position(x: max(0, xPosition(for: similarValue, in: width)) / 2, y: centerY - 48)
                
                // Bottom Bracket for Duplicates
                Path { path in
                    let endX = max(0, xPosition(for: duplicateValue, in: width))
                    let startY = centerY + 22
                    let midY = centerY + 30
                    let bottomY = centerY + 38
                    
                    if endX > 10 {
                        path.move(to: CGPoint(x: 0, y: startY))
                        path.addLine(to: CGPoint(x: 0, y: midY))
                        path.addLine(to: CGPoint(x: endX/2 - 5, y: midY))
                        path.addLine(to: CGPoint(x: endX/2, y: bottomY))
                        path.addLine(to: CGPoint(x: endX/2 + 5, y: midY))
                        path.addLine(to: CGPoint(x: endX, y: midY))
                        path.addLine(to: CGPoint(x: endX, y: startY))
                    }
                }
                .stroke(Color.secondary, lineWidth: 1)
                
                Text("Duplicates")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .position(x: max(0, xPosition(for: duplicateValue, in: width)) / 2, y: centerY + 48)
                
                // Similar Images Thumb (Pointing Down)
                Path { path in
                    path.move(to: CGPoint(x: 0, y: 0))
                    path.addLine(to: CGPoint(x: thumbSize, y: 0))
                    path.addLine(to: CGPoint(x: thumbSize/2, y: thumbSize))
                    path.closeSubpath()
                }
                .fill(Color.white)
                .overlay(
                    Path { path in
                        path.move(to: CGPoint(x: 0, y: 0))
                        path.addLine(to: CGPoint(x: thumbSize, y: 0))
                        path.addLine(to: CGPoint(x: thumbSize/2, y: thumbSize))
                        path.closeSubpath()
                    }
                    .stroke(Color.gray, lineWidth: 1)
                )
                .shadow(radius: 1)
                .frame(width: thumbSize, height: thumbSize)
                .offset(x: xPosition(for: similarValue, in: width) - thumbSize/2, y: centerY - thumbSize - 2)
                .gesture(
                    DragGesture()
                        .onChanged { value in
                            let newValue = valueFor(x: value.location.x, in: width)
                            similarValue = min(max(range.lowerBound, newValue), range.upperBound)
                            if similarValue < duplicateValue {
                                duplicateValue = similarValue
                            }
                        }
                )
                
                // Duplicates Thumb (Pointing Up)
                Path { path in
                    path.move(to: CGPoint(x: thumbSize/2, y: 0))
                    path.addLine(to: CGPoint(x: thumbSize, y: thumbSize))
                    path.addLine(to: CGPoint(x: 0, y: thumbSize))
                    path.closeSubpath()
                }
                .fill(Color.white)
                .overlay(
                    Path { path in
                        path.move(to: CGPoint(x: thumbSize/2, y: 0))
                        path.addLine(to: CGPoint(x: thumbSize, y: thumbSize))
                        path.addLine(to: CGPoint(x: 0, y: thumbSize))
                        path.closeSubpath()
                    }
                    .stroke(Color.gray, lineWidth: 1)
                )
                .shadow(radius: 1)
                .frame(width: thumbSize, height: thumbSize)
                .offset(x: xPosition(for: duplicateValue, in: width) - thumbSize/2, y: centerY + 2)
                .gesture(
                    DragGesture()
                        .onChanged { value in
                            let newValue = valueFor(x: value.location.x, in: width)
                            duplicateValue = min(max(range.lowerBound, newValue), range.upperBound)
                            if duplicateValue > similarValue {
                                similarValue = duplicateValue
                            }
                        }
                )
            }
        }
    }
    
    private func xPosition(for value: Double, in width: CGFloat) -> CGFloat {
        let percentage = (value - range.lowerBound) / (range.upperBound - range.lowerBound)
        return CGFloat(percentage) * width
    }
    
    private func valueFor(x: CGFloat, in width: CGFloat) -> Double {
        let percentage = Double(x / width)
        return range.lowerBound + percentage * (range.upperBound - range.lowerBound)
    }
}
