import SwiftUI

struct VurioScrollerTestView: View {
    // The scroller is uniform-height by design (closed-form geometry); the
    // harness exercises keyboard/scroll/focus interaction at 100k rows.
    private let itemCount = 100_000
    private let rowHeight: CGFloat = 150
    @State private var selection: Int? = 0

    var body: some View {
        VStack(spacing: 0) {
            Text("Vurio Scroller Test Harness")
                .font(.title2)
                .fontWeight(.bold)
                .padding()
                .frame(maxWidth: .infinity)
                .background(Color.black.opacity(0.3))

            GeometryReader { geo in
                VurioScroller(
                    itemCount: itemCount,
                    rowHeight: rowHeight,
                    scrollbarInsets: geo.safeAreaInsets,
                    selection: $selection
                ) { index in
                    TestRowView(index: index, height: rowHeight)
                }
                .background(Color.black.opacity(0.1))
            }
        }
    }
}

struct TestRowView: View {
    let index: Int
    let height: CGFloat
    @State private var text: String = ""
    @State private var selectedTab = 0
    
    var body: some View {
        HStack(spacing: 20) {
            Text("Group #\(index)")
                .font(.title3)
                .fontWeight(.semibold)
                .frame(width: 120, alignment: .leading)
            
            VStack(alignment: .leading, spacing: 10) {
                // Test 1: Typing text should intercept spacebar and arrow keys naturally
                TextField("Edit Description...", text: $text)
                    .textFieldStyle(RoundedBorderTextFieldStyle())
                    .frame(width: 300)
                
                // Test 2: Arrow keys should navigate segmented control, NOT scroll the page
                Picker("Options", selection: $selectedTab) {
                    Text("Details").tag(0)
                    Text("Metadata").tag(1)
                    Text("Actions").tag(2)
                }
                .pickerStyle(SegmentedPickerStyle())
                .frame(width: 300)
            }
            
            Spacer()
            
            Image(systemName: "photo.on.rectangle")
                .font(.system(size: 40))
                .foregroundColor(.gray)
        }
        .padding(.horizontal, 24)
        .frame(height: height)
        .background(index % 2 == 0 ? Color.white.opacity(0.02) : Color.white.opacity(0.05))
        // Explicitly clip to prevent weird layout overdraw
        .clipped()
    }
}
